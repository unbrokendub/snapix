#include "PageCache.h"

#include <Logging.h>

#define TAG "CACHE"

#include <Page.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <esp_timer.h>

#include <cstdarg>
#include <cstdio>

#include "ContentParser.h"

namespace {
constexpr uint8_t CACHE_FILE_VERSION = 20;  // v20: serialize image provenance for EPUB fallback tracing
constexpr uint16_t MAX_REASONABLE_PAGE_COUNT = 8192;

#ifndef PAPYRIX_PERF_LOG
#define PAPYRIX_PERF_LOG 0
#endif

inline uint32_t perfMsNow() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

inline void perfLog(const char* phase, uint32_t startedMs, const char* fmt = nullptr, ...) {
#if PAPYRIX_PERF_LOG
  char suffix[96] = "";
  if (fmt) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(suffix, sizeof(suffix), fmt, args);
    va_end(args);
  }
  LOG_INF(TAG, "[PERF] %s: %lu ms%s%s", phase, static_cast<unsigned long>(perfMsNow() - startedMs),
          suffix[0] ? " " : "", suffix);
#else
  (void)phase;
  (void)startedMs;
  (void)fmt;
#endif
}

// Header layout:
// - version (1 byte)
// - fontId (4 bytes)
// - lineCompression (4 bytes)
// - indentLevel (1 byte)
// - spacingLevel (1 byte)
// - paragraphAlignment (1 byte)
// - hyphenation (1 byte)
// - showImages (1 byte)
// - viewportWidth (2 bytes)
// - viewportHeight (2 bytes)
// - pageCount (2 bytes)
// - isPartial (1 byte)
// - lutOffset (4 bytes)
constexpr uint32_t HEADER_SIZE = 1 + 4 + 4 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 2 + 1 + 4;

bool validateCacheIndexBounds(const char* cachePath, const size_t fileSize, const uint16_t pageCount,
                              const uint32_t lutOffset) {
  if (pageCount == 0) {
    LOG_ERR(TAG, "Rejecting empty/incomplete cache file: %s", cachePath);
    return false;
  }

  if (pageCount > MAX_REASONABLE_PAGE_COUNT) {
    LOG_ERR(TAG, "Rejecting cache with implausible page count %u: %s", static_cast<unsigned>(pageCount), cachePath);
    return false;
  }

  if (lutOffset < HEADER_SIZE || lutOffset >= fileSize) {
    LOG_ERR(TAG, "Invalid lutOffset: %u (file size: %zu)", static_cast<unsigned>(lutOffset), fileSize);
    return false;
  }

  const uint64_t lutBytes = static_cast<uint64_t>(pageCount) * sizeof(uint32_t);
  const uint64_t lutEnd = static_cast<uint64_t>(lutOffset) + lutBytes;
  if (lutEnd > fileSize) {
    LOG_ERR(TAG, "Rejecting cache with truncated LUT: path=%s pages=%u lutOffset=%u fileSize=%zu", cachePath,
            static_cast<unsigned>(pageCount), static_cast<unsigned>(lutOffset), fileSize);
    return false;
  }

  return true;
}

bool evictCacheFile(const std::string& cachePath, const char* reason) {
  if (!SdMan.exists(cachePath.c_str())) {
    return true;
  }

  if (SdMan.remove(cachePath.c_str())) {
    LOG_ERR(TAG, "Removed stale cache file reason=%s path=%s", reason ? reason : "unknown", cachePath.c_str());
    return true;
  }

  const std::string quarantinePath = cachePath + ".stale";
  if (SdMan.exists(quarantinePath.c_str())) {
    SdMan.remove(quarantinePath.c_str());
  }

  if (SdMan.rename(cachePath.c_str(), quarantinePath.c_str())) {
    LOG_ERR(TAG, "Quarantined stale cache file reason=%s path=%s quarantine=%s", reason ? reason : "unknown",
            cachePath.c_str(), quarantinePath.c_str());
    return true;
  }

  LOG_ERR(TAG, "Failed to evict stale cache file reason=%s path=%s", reason ? reason : "unknown", cachePath.c_str());
  return false;
}
}  // namespace

PageCache::PageCache(std::string cachePath) : cachePath_(std::move(cachePath)) {}

bool PageCache::ensureReadHandle() {
  if (readFile_) return true;
  if (!SdMan.openFileForRead("CACHE", cachePath_, readFile_)) {
    return false;
  }
  readFileSize_ = readFile_.size();
  return true;
}

void PageCache::closeReadHandle() {
  if (readFile_) {
    readFile_.close();
  }
  readFileSize_ = 0;
}

std::shared_ptr<Page> PageCache::getResidentPage(uint16_t pageNum) {
  for (auto& entry : residentPages_) {
    if (entry.pageNum == pageNum && entry.page) {
      entry.useToken = ++residentUseClock_;
      return entry.page;
    }
  }
  return nullptr;
}

void PageCache::putResidentPage(uint16_t pageNum, std::shared_ptr<Page> page) {
  if (!page) return;

  for (auto& entry : residentPages_) {
    if (entry.pageNum == pageNum) {
      entry.page = std::move(page);
      entry.useToken = ++residentUseClock_;
      return;
    }
  }

  if (residentPages_.size() >= RESIDENT_PAGE_LIMIT) {
    auto lruIt = residentPages_.begin();
    for (auto it = residentPages_.begin() + 1; it != residentPages_.end(); ++it) {
      if (it->useToken < lruIt->useToken) lruIt = it;
    }
    *lruIt = ResidentPage{pageNum, ++residentUseClock_, std::move(page)};
    return;
  }

  residentPages_.push_back(ResidentPage{pageNum, ++residentUseClock_, std::move(page)});
}

bool PageCache::ensureLutLoaded() {
  if (!pageLut_.empty()) return true;
  std::vector<uint32_t> lut;
  return loadLut(lut);
}

void PageCache::clearResidentPages() {
  residentPages_.clear();
  residentUseClock_ = 0;
}

void PageCache::trimResidentPages(uint16_t centerPage, uint8_t keepBehind, uint8_t keepAhead) {
  if (residentPages_.empty()) return;

  const int minPage = std::max(0, static_cast<int>(centerPage) - static_cast<int>(keepBehind));
  const int maxPage = static_cast<int>(centerPage) + static_cast<int>(keepAhead);

  residentPages_.erase(std::remove_if(residentPages_.begin(), residentPages_.end(),
                                      [minPage, maxPage](const ResidentPage& entry) {
                                        return !entry.page || static_cast<int>(entry.pageNum) < minPage ||
                                               static_cast<int>(entry.pageNum) > maxPage;
                                      }),
                       residentPages_.end());

  if (residentPages_.empty()) {
    residentUseClock_ = 0;
  }
}

bool PageCache::writeHeader(bool isPartial) {
  file_.seek(0);
  serialization::writePod(file_, CACHE_FILE_VERSION);
  serialization::writePod(file_, config_.fontId);
  serialization::writePod(file_, config_.lineCompression);
  serialization::writePod(file_, config_.indentLevel);
  serialization::writePod(file_, config_.spacingLevel);
  serialization::writePod(file_, config_.paragraphAlignment);
  serialization::writePod(file_, config_.hyphenation);
  serialization::writePod(file_, config_.showImages);
  serialization::writePod(file_, config_.viewportWidth);
  serialization::writePod(file_, config_.viewportHeight);
  serialization::writePod(file_, pageCount_);
  serialization::writePod(file_, static_cast<uint8_t>(isPartial ? 1 : 0));
  serialization::writePod(file_, static_cast<uint32_t>(0));  // LUT offset placeholder
  return true;
}

bool PageCache::writeLut(const std::vector<uint32_t>& lut) {
  const uint32_t lutOffset = file_.position();

  for (const uint32_t pos : lut) {
    if (pos == 0) {
      LOG_ERR(TAG, "Invalid page position in LUT");
      return false;
    }
    serialization::writePod(file_, pos);
  }

  // Update header with final values
  file_.seek(HEADER_SIZE - 4 - 1 - 2);  // Seek to pageCount
  serialization::writePod(file_, pageCount_);
  serialization::writePod(file_, static_cast<uint8_t>(isPartial_ ? 1 : 0));
  serialization::writePod(file_, lutOffset);
  lutOffset_ = lutOffset;
  pageLut_ = lut;
  clearResidentPages();

  return true;
}

bool PageCache::loadLut(std::vector<uint32_t>& lut) {
  if (!ensureReadHandle()) {
    return false;
  }

  const size_t fileSize = readFileSize_;
  if (fileSize < HEADER_SIZE) {
    LOG_ERR(TAG, "File too small: %zu (need %u)", fileSize, HEADER_SIZE);
    closeReadHandle();
    return false;
  }

  // Read lutOffset from header
  readFile_.seek(HEADER_SIZE - 4);
  serialization::readPod(readFile_, lutOffset_);

  // Validate lutOffset before seeking
  if (lutOffset_ < HEADER_SIZE || lutOffset_ >= fileSize) {
    LOG_ERR(TAG, "Invalid lutOffset: %u (file size: %zu)", lutOffset_, fileSize);
    closeReadHandle();
    return false;
  }

  // Read pageCount from header
  readFile_.seek(HEADER_SIZE - 4 - 1 - 2);
  serialization::readPod(readFile_, pageCount_);

  if (!validateCacheIndexBounds(cachePath_.c_str(), fileSize, pageCount_, lutOffset_)) {
    closeReadHandle();
    return false;
  }

  // Read existing LUT entries
  readFile_.seek(lutOffset_);
  lut.clear();
  lut.reserve(pageCount_);
  for (uint16_t i = 0; i < pageCount_; i++) {
    uint32_t pos;
    serialization::readPod(readFile_, pos);
    lut.push_back(pos);
  }
  pageLut_ = lut;
  clearResidentPages();
  return true;
}

bool PageCache::loadRaw() {
  if (!ensureReadHandle()) {
    return false;
  }

  readFile_.seek(0);
  uint8_t version;
  serialization::readPod(readFile_, version);
  if (version != CACHE_FILE_VERSION) {
    closeReadHandle();
    LOG_ERR(TAG, "Version mismatch: got %u, expected %u", version, CACHE_FILE_VERSION);
    return false;
  }

  // Skip config fields, read pageCount and isPartial
  readFile_.seek(HEADER_SIZE - 4 - 1 - 2);
  serialization::readPod(readFile_, pageCount_);
  if (pageCount_ == 0) {
    closeReadHandle();
    LOG_ERR(TAG, "Rejecting empty cache file: %s", cachePath_.c_str());
    return false;
  }
  uint8_t partial;
  serialization::readPod(readFile_, partial);
  isPartial_ = (partial != 0);
  readFile_.seek(HEADER_SIZE - 4);
  serialization::readPod(readFile_, lutOffset_);

  if (!validateCacheIndexBounds(cachePath_.c_str(), readFileSize_, pageCount_, lutOffset_)) {
    closeReadHandle();
    pageCount_ = 0;
    isPartial_ = false;
    return false;
  }

  pageLut_.clear();
  clearResidentPages();
  return true;
}

PageCache::ProbeResult PageCache::probe(const std::string& cachePath, const RenderConfig& config, bool cleanupInvalid) {
  ProbeResult result;
  if (!SdMan.exists(cachePath.c_str())) {
    return result;
  }

  result.exists = true;

  FsFile readFile;
  if (!SdMan.openFileForRead("CACHE", cachePath, readFile)) {
    return result;
  }

  const size_t fileSize = readFile.size();
  if (fileSize < HEADER_SIZE) {
    readFile.close();
    if (cleanupInvalid) {
      SdMan.remove(cachePath.c_str());
    }
    return result;
  }

  uint8_t version;
  serialization::readPod(readFile, version);
  if (version != CACHE_FILE_VERSION) {
    readFile.close();
    if (cleanupInvalid) {
      SdMan.remove(cachePath.c_str());
    }
    return result;
  }

  RenderConfig fileConfig;
  serialization::readPod(readFile, fileConfig.fontId);
  serialization::readPod(readFile, fileConfig.lineCompression);
  serialization::readPod(readFile, fileConfig.indentLevel);
  serialization::readPod(readFile, fileConfig.spacingLevel);
  serialization::readPod(readFile, fileConfig.paragraphAlignment);
  serialization::readPod(readFile, fileConfig.hyphenation);
  serialization::readPod(readFile, fileConfig.showImages);
  serialization::readPod(readFile, fileConfig.viewportWidth);
  serialization::readPod(readFile, fileConfig.viewportHeight);
  if (config != fileConfig) {
    readFile.close();
    if (cleanupInvalid) {
      SdMan.remove(cachePath.c_str());
    }
    return result;
  }

  serialization::readPod(readFile, result.pageCount);
  uint8_t partial;
  serialization::readPod(readFile, partial);
  result.partial = (partial != 0);

  uint32_t lutOffset = 0;
  serialization::readPod(readFile, lutOffset);
  readFile.close();

  if (!validateCacheIndexBounds(cachePath.c_str(), fileSize, result.pageCount, lutOffset)) {
    if (cleanupInvalid) {
      SdMan.remove(cachePath.c_str());
    }
    result.partial = false;
    result.pageCount = 0;
    return result;
  }

  result.valid = true;
  return result;
}

bool PageCache::load(const RenderConfig& config) {
  if (!ensureReadHandle()) {
    return false;
  }

  // Read and validate header
  readFile_.seek(0);
  uint8_t version;
  serialization::readPod(readFile_, version);
  if (version != CACHE_FILE_VERSION) {
    closeReadHandle();
    LOG_ERR(TAG, "Version mismatch: got %u, expected %u", version, CACHE_FILE_VERSION);
    clear();
    pageLut_.clear();
    clearResidentPages();
    return false;
  }

  RenderConfig fileConfig;
  serialization::readPod(readFile_, fileConfig.fontId);
  serialization::readPod(readFile_, fileConfig.lineCompression);
  serialization::readPod(readFile_, fileConfig.indentLevel);
  serialization::readPod(readFile_, fileConfig.spacingLevel);
  serialization::readPod(readFile_, fileConfig.paragraphAlignment);
  serialization::readPod(readFile_, fileConfig.hyphenation);
  serialization::readPod(readFile_, fileConfig.showImages);
  serialization::readPod(readFile_, fileConfig.viewportWidth);
  serialization::readPod(readFile_, fileConfig.viewportHeight);

  if (config != fileConfig) {
    closeReadHandle();
    LOG_INF(TAG, "Config mismatch, invalidating cache");
    clear();
    pageLut_.clear();
    clearResidentPages();
    return false;
  }

  serialization::readPod(readFile_, pageCount_);
  uint8_t partial;
  serialization::readPod(readFile_, partial);
  isPartial_ = (partial != 0);
  serialization::readPod(readFile_, lutOffset_);

  if (!validateCacheIndexBounds(cachePath_.c_str(), readFileSize_, pageCount_, lutOffset_)) {
    closeReadHandle();
    clear();
    pageCount_ = 0;
    isPartial_ = false;
    pageLut_.clear();
    clearResidentPages();
    return false;
  }

  config_ = config;
  pageLut_.clear();
  clearResidentPages();
  LOG_INF(TAG, "Loaded: %d pages, partial=%d", pageCount_, isPartial_);
  return true;
}

bool PageCache::create(ContentParser& parser, const RenderConfig& config, uint16_t maxPages, uint16_t skipPages,
                       const AbortCallback& shouldAbort) {
  LOG_ERR(TAG, "[CACHE] create start path=%s maxPages=%u skipPages=%u", cachePath_.c_str(),
          static_cast<unsigned>(maxPages), static_cast<unsigned>(skipPages));
  const uint32_t startMs = perfMsNow();

  std::vector<uint32_t> lut;
  const bool isExtendPass = skipPages > 0;
  uint16_t initialPageCount = 0;

  if (skipPages > 0) {
    // Extending: load existing LUT
    if (!loadLut(lut)) {
      LOG_ERR(TAG, "Failed to load existing LUT for extend");
      return false;
    }
    initialPageCount = pageCount_;

    // Append new pages AFTER old LUT (crash-safe: old LUT remains valid until header update)
    closeReadHandle();
    if (!file_.open(cachePath_.c_str(), O_RDWR)) {
      LOG_ERR(TAG, "Failed to open cache file for append");
      return false;
    }
    file_.seekEnd();  // Append after old LUT
  } else {
    // Fresh create
    closeReadHandle();
    if (file_) {
      file_.close();
    }
    if (!SdMan.openFileForWrite("CACHE", cachePath_, file_)) {
      if (SdMan.exists(cachePath_.c_str())) {
        LOG_ERR(TAG, "Retrying cache create after removing stale file: %s", cachePath_.c_str());
        evictCacheFile(cachePath_, "fresh-create-open-failed");
        if (!SdMan.openFileForWrite("CACHE", cachePath_, file_)) {
          LOG_ERR(TAG, "Failed to open cache file for writing");
          return false;
        }
      } else {
        LOG_ERR(TAG, "Failed to open cache file for writing");
        return false;
      }
    }

    config_ = config;
    pageCount_ = 0;
    isPartial_ = false;
    pageLut_.clear();
    clearResidentPages();
    initialPageCount = 0;

    // Write placeholder header
    writeHeader(false);
  }

  // Check for abort before starting expensive parsing
  if (shouldAbort && shouldAbort()) {
    file_.close();
    // Fresh create writes a placeholder header before parsing starts. If we abort
    // here and keep that file, later probes see a zero-page cache and spin on
    // "Rejecting empty/incomplete cache file" for the same section.
    if (!isExtendPass) {
      SdMan.remove(cachePath_.c_str());
    }
    LOG_INF(TAG, "Aborted before parsing");
    return false;
  }

  uint16_t parsedPages = 0;
  bool hitMaxPages = false;
  bool aborted = false;

  bool success = parser.parsePages(
      [this, &lut, &hitMaxPages, &parsedPages, maxPages, skipPages](std::unique_ptr<Page> page) {
        if (hitMaxPages) return;

        parsedPages++;

        // Skip pages we already have cached
        if (parsedPages <= skipPages) {
          return;
        }

        // Serialize new page
        const uint32_t position = file_.position();
        if (!page->serialize(file_)) {
          LOG_ERR(TAG, "Failed to serialize page %d", pageCount_);
          return;
        }

        lut.push_back(position);
        pageCount_++;
        LOG_DBG(TAG, "Page %d cached", pageCount_ - 1);

        if (maxPages > 0 && pageCount_ >= maxPages) {
          hitMaxPages = true;
        }
      },
      maxPages, shouldAbort);

  // Check if we were aborted
  if (shouldAbort && shouldAbort()) {
    aborted = true;
    LOG_INF(TAG, "Aborted during parsing");
  }

  const bool madeForwardProgress = pageCount_ > initialPageCount;

  if (!success && pageCount_ == 0) {
    file_.close();
    // Remove file to prevent corrupt/incomplete cache
    evictCacheFile(cachePath_, "create-empty-failure");
    LOG_ERR(TAG, "[CACHE] create failed/aborted path=%s pages=%u success=%u aborted=%u", cachePath_.c_str(),
            static_cast<unsigned>(pageCount_), static_cast<unsigned>(success), static_cast<unsigned>(aborted));
    return false;
  }

  if (aborted || !success) {
    if (!madeForwardProgress) {
      file_.close();

      // During extend/rebuild passes the old header/LUT still points at the last
      // known-good cache contents. Keep that file instead of deleting the whole
      // section cache when the new pass gets aborted.
      if (isExtendPass && initialPageCount > 0) {
        pageCount_ = initialPageCount;
        isPartial_ = true;
        pageLut_ = lut;
        clearResidentPages();
        LOG_ERR(TAG,
                "[CACHE] create kept previous cache path=%s pages=%u success=%u aborted=%u",
                cachePath_.c_str(), static_cast<unsigned>(pageCount_), static_cast<unsigned>(success),
                static_cast<unsigned>(aborted));
        return true;
      }

      evictCacheFile(cachePath_, "create-no-progress");
      LOG_ERR(TAG, "[CACHE] create failed/aborted path=%s pages=%u success=%u aborted=%u",
              cachePath_.c_str(), static_cast<unsigned>(pageCount_), static_cast<unsigned>(success),
              static_cast<unsigned>(aborted));
      return false;
    }

    // We already serialized some new pages. Finalize the cache as partial so the
    // reader can keep using the progress made so far instead of throwing it away.
    isPartial_ = true;
    if (!writeLut(lut)) {
      file_.close();
      if (!isExtendPass) {
        evictCacheFile(cachePath_, "create-partial-write-lut-failed");
      }
      return false;
    }

    file_.close();
    LOG_ERR(TAG, "[CACHE] create partial path=%s pages=%u success=%u aborted=%u", cachePath_.c_str(),
            static_cast<unsigned>(pageCount_), static_cast<unsigned>(success), static_cast<unsigned>(aborted));
    perfLog("cache-create", startMs, "(pages=%u partial=%u)", pageCount_, static_cast<unsigned>(isPartial_));
    return true;
  }

  isPartial_ = parser.hasMoreContent();
  if (!madeForwardProgress && isPartial_) {
    file_.close();
    if (isExtendPass && initialPageCount > 0) {
      pageCount_ = initialPageCount;
      pageLut_ = lut;
      clearResidentPages();
      LOG_ERR(TAG, "[CACHE] create kept previous cache path=%s pages=%u reason=no-progress-partial",
              cachePath_.c_str(), static_cast<unsigned>(pageCount_));
      return true;
    }
    evictCacheFile(cachePath_, "create-no-progress-partial");
    LOG_ERR(TAG, "[CACHE] create failed/aborted path=%s pages=%u success=%u aborted=%u reason=no-progress-partial",
            cachePath_.c_str(), static_cast<unsigned>(pageCount_), static_cast<unsigned>(success),
            static_cast<unsigned>(aborted));
    return false;
  }

  if (!writeLut(lut)) {
    file_.close();
    evictCacheFile(cachePath_, "create-write-lut-failed");
    return false;
  }

  file_.close();
  LOG_ERR(TAG, "[CACHE] create done path=%s pages=%u partial=%u", cachePath_.c_str(),
          static_cast<unsigned>(pageCount_), static_cast<unsigned>(isPartial_));
  perfLog("cache-create", startMs, "(pages=%u partial=%u)", pageCount_, static_cast<unsigned>(isPartial_));
  return true;
}

bool PageCache::extend(ContentParser& parser, uint16_t additionalPages, const AbortCallback& shouldAbort) {
  LOG_ERR(TAG, "[CACHE] extend start path=%s current=%u add=%u partial=%u canResume=%u", cachePath_.c_str(),
          static_cast<unsigned>(pageCount_), static_cast<unsigned>(additionalPages), static_cast<unsigned>(isPartial_),
          static_cast<unsigned>(parser.canResume()));
  if (!isPartial_) {
    LOG_INF(TAG, "Cache is complete, nothing to extend");
    return true;
  }

  // Large fixed jumps are fine only when the parser can resume in-place.
  // On cold extend paths (FB2/TXT or rebuilt EPUB parser) they cause long
  // "indexing" bursts because we end up reparsing from the start just to
  // skip already cached pages. Respect the caller's requested batch there.
  const uint16_t chunk =
      parser.canResume() ? (pageCount_ >= 30 ? std::min<uint16_t>(50, additionalPages) : additionalPages)
                         : additionalPages;
  const uint16_t currentPages = pageCount_;

  if (parser.canResume()) {
    // HOT PATH: Parser has live session from previous extend, just append new pages.
    // No re-parsing — O(chunk) work instead of O(totalPages).
    LOG_INF(TAG, "Hot extend from %d pages (+%d)", currentPages, chunk);

    std::vector<uint32_t> lut;
    if (!loadLut(lut)) return false;

    closeReadHandle();
    bool opened = false;
    for (int attempt = 0; attempt < 3; attempt++) {
      if (attempt > 0) delay(50);
      if (file_.open(cachePath_.c_str(), O_RDWR)) {
        opened = true;
        break;
      }
    }
    if (!opened) {
      LOG_ERR(TAG, "Failed to open cache file for hot extend");
      return false;
    }
    file_.seekEnd();

    const uint16_t pagesBefore = pageCount_;
    bool parseOk = parser.parsePages(
        [this, &lut](std::unique_ptr<Page> page) {
          const uint32_t position = file_.position();
          if (!page->serialize(file_)) return;
          lut.push_back(position);
          pageCount_++;
        },
        chunk, shouldAbort);

    isPartial_ = parser.hasMoreContent();

    if (!parseOk && pageCount_ == pagesBefore) {
      file_.close();
      LOG_ERR(TAG, "[CACHE] hot extend failed path=%s pagesBefore=%u", cachePath_.c_str(),
              static_cast<unsigned>(pagesBefore));
      return false;
    }

    if (!writeLut(lut)) {
      file_.close();
      SdMan.remove(cachePath_.c_str());
      return false;
    }

    file_.close();
    LOG_ERR(TAG, "[CACHE] hot extend done path=%s pages=%u partial=%u", cachePath_.c_str(),
            static_cast<unsigned>(pageCount_), static_cast<unsigned>(isPartial_));
    LOG_INF(TAG, "Hot extend done: %d pages, partial=%d", pageCount_, isPartial_);
    return true;
  }

  // COLD PATH: Fresh parser (after exit/reboot) — re-parse from start.
  //
  // Reusing the old "skip already cached pages and append new ones" strategy is
  // unsafe for content whose pagination can change while images/background work
  // complete. That produced mixed caches where the early pages were kept from an
  // older image-less layout and only the tail reflected the rebuilt pagination.
  //
  // Rebuild the whole section into a temporary cache and promote it only if the
  // new file preserves at least the already-visible page count. This keeps the
  // previous cache usable when an aborted rebuild fails to catch up.
  const uint16_t targetPages = pageCount_ + chunk;
  LOG_INF(TAG, "Cold extend from %d to %d pages", currentPages, targetPages);

  parser.reset();
  const std::string rebuildPath = cachePath_ + ".rebuild";
  if (SdMan.exists(rebuildPath.c_str())) {
    SdMan.remove(rebuildPath.c_str());
  }

  PageCache rebuiltCache(rebuildPath);
  const bool rebuildResult = rebuiltCache.create(parser, config_, targetPages, 0, shouldAbort);
  const bool canPromoteRebuild = rebuildResult && rebuiltCache.pageCount() >= currentPages;

  if (!canPromoteRebuild) {
    // The parser state now belongs to the discarded rebuild lineage, not to the
    // currently active cache file. Keeping it resumable here would allow a later
    // hot-extend to append pages from an incompatible parser position onto the
    // old cache, corrupting section pagination.
    parser.reset();
    if (SdMan.exists(rebuildPath.c_str())) {
      SdMan.remove(rebuildPath.c_str());
    }
    LOG_ERR(TAG, "[CACHE] cold extend kept previous path=%s rebuilt=%u rebuiltPages=%u current=%u",
            cachePath_.c_str(), static_cast<unsigned>(rebuildResult), static_cast<unsigned>(rebuiltCache.pageCount()),
            static_cast<unsigned>(currentPages));
    return true;
  }

  closeReadHandle();
  if (file_) {
    file_.close();
  }
  clearResidentPages();
  pageLut_.clear();

  const std::string backupPath = cachePath_ + ".bak";
  if (SdMan.exists(backupPath.c_str())) {
    SdMan.remove(backupPath.c_str());
  }

  const bool hadOldCache = SdMan.exists(cachePath_.c_str());
  if (hadOldCache && !SdMan.rename(cachePath_.c_str(), backupPath.c_str())) {
    LOG_ERR(TAG, "Failed to back up old cache before cold promote: %s", cachePath_.c_str());
    SdMan.remove(rebuildPath.c_str());
    return false;
  }

  if (!SdMan.rename(rebuildPath.c_str(), cachePath_.c_str())) {
    LOG_ERR(TAG, "Failed to promote rebuilt cache: %s", cachePath_.c_str());
    SdMan.remove(rebuildPath.c_str());
    if (hadOldCache) {
      SdMan.rename(backupPath.c_str(), cachePath_.c_str());
    }
    return false;
  }

  const bool result = load(config_);
  if (!result) {
    LOG_ERR(TAG, "Failed to reload promoted cache: %s", cachePath_.c_str());
    SdMan.remove(cachePath_.c_str());
    if (hadOldCache) {
      SdMan.rename(backupPath.c_str(), cachePath_.c_str());
      if (load(config_)) {
        return true;
      }
    }
    return false;
  }

  if (hadOldCache && SdMan.exists(backupPath.c_str())) {
    SdMan.remove(backupPath.c_str());
  }

  // No forward progress AND parser has no more content → content is truly finished.
  // Without the hasMoreContent() check, an aborted extend (timeout/memory pressure)
  // would permanently mark the chapter as complete, truncating it.
  if (result && pageCount_ <= currentPages && !parser.hasMoreContent()) {
    LOG_INF(TAG, "No progress during extend (%d pages), marking complete", pageCount_);
    isPartial_ = false;
  }

  LOG_ERR(TAG, "[CACHE] cold extend done path=%s result=%u pages=%u partial=%u", cachePath_.c_str(),
          static_cast<unsigned>(result), static_cast<unsigned>(pageCount_), static_cast<unsigned>(isPartial_));
  return result;
}

std::shared_ptr<Page> PageCache::loadPage(uint16_t pageNum) {
  if (pageNum >= pageCount_) {
    LOG_ERR(TAG, "Page %d out of range (max %d)", pageNum, pageCount_);
    return nullptr;
  }

  const uint32_t startMs = perfMsNow();

  if (auto resident = getResidentPage(pageNum)) {
    perfLog("page-load", startMs, "(page=%u source=resident resident=%zu)", pageNum, residentPages_.size());
    return resident;
  }

  if (!ensureLutLoaded()) return nullptr;

  if (pageNum >= pageLut_.size()) {
    LOG_ERR(TAG, "Page LUT missing entry %d (size %zu)", pageNum, pageLut_.size());
    return nullptr;
  }

  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) delay(50);

    if (!ensureReadHandle()) {
      continue;
    }
    const size_t fileSize = readFileSize_;
    const uint32_t pagePos = pageLut_[pageNum];

    // Validate page position
    if (pagePos < HEADER_SIZE || pagePos >= fileSize) {
      LOG_ERR(TAG, "Invalid page position: %u (file size: %zu)", pagePos, fileSize);
      closeReadHandle();
      continue;
    }

    // Read page
    readFile_.seek(pagePos);
    auto page = Page::deserialize(readFile_);

    if (page) {
      auto sharedPage = std::shared_ptr<Page>(std::move(page));
      putResidentPage(pageNum, sharedPage);
      perfLog("page-load", startMs, "(page=%u source=sd resident=%zu)", pageNum, residentPages_.size());
      return sharedPage;
    }
  }

  return nullptr;
}

void PageCache::prefetchWindow(uint16_t centerPage, int direction, uint8_t span) {
  if (pageCount_ == 0 || !ensureLutLoaded()) return;
  if (span == 0) {
    trimResidentPages(centerPage, 0, 0);
    return;
  }

  std::vector<uint16_t> wanted;
  wanted.reserve(span + 1);

  const uint8_t forwardBias = direction >= 0 ? span : 1;
  const uint8_t backwardBias = direction >= 0 ? 1 : span;

  trimResidentPages(centerPage, backwardBias, forwardBias);

  for (uint8_t delta = 1; delta <= forwardBias; ++delta) {
    const int page = centerPage + delta;
    if (page >= 0 && page < pageCount_) wanted.push_back(static_cast<uint16_t>(page));
  }
  for (uint8_t delta = 1; delta <= backwardBias; ++delta) {
    const int page = static_cast<int>(centerPage) - delta;
    if (page >= 0 && page < pageCount_) wanted.push_back(static_cast<uint16_t>(page));
  }

  bool hasMiss = false;
  for (uint16_t pageNum : wanted) {
    if (!getResidentPage(pageNum)) {
      hasMiss = true;
      break;
    }
  }
  if (!hasMiss) return;

  const uint32_t startMs = perfMsNow();
  if (!ensureReadHandle()) return;

  const size_t fileSize = readFileSize_;
  for (uint16_t pageNum : wanted) {
    if (getResidentPage(pageNum)) continue;
    if (pageNum >= pageLut_.size()) continue;

    const uint32_t pagePos = pageLut_[pageNum];
    if (pagePos < HEADER_SIZE || pagePos >= fileSize) continue;

    readFile_.seek(pagePos);
    auto page = Page::deserialize(readFile_);
    if (page) {
      putResidentPage(pageNum, std::shared_ptr<Page>(std::move(page)));
    }
  }
  perfLog("page-prefetch", startMs, "(center=%u dir=%d resident=%zu)", centerPage, direction, residentPages_.size());
}

bool PageCache::clear() {
  clearResidentPages();
  pageLut_.clear();
  closeReadHandle();
  if (file_) {
    file_.close();
  }
  pageCount_ = 0;
  isPartial_ = false;
  lutOffset_ = 0;
  readFileSize_ = 0;
  if (!SdMan.exists(cachePath_.c_str())) {
    return true;
  }
  return evictCacheFile(cachePath_, "clear");
}
