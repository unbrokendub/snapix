#include "PageCache.h"

#include <Logging.h>

#define TAG "CACHE"

#include <JpegToBmpConverter.h>  // v2.0.81: releaseAllPersistent() in cold-extend pre-flight
#include <LittleFS.h>
#include <Page.h>
#include <Serialization.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cstdarg>
#include <cstdio>
#include <new>

#include "ContentParser.h"

namespace {
constexpr uint8_t CACHE_FILE_VERSION = 29;  // v29: v2.0.60 page cache moved from SD to LittleFS.  Cache file paths now resolve under /cache/<book_hash>/sections/ on internal flash; ImageBlock cachedBmpPath, RenderConfig serialization layout, and PageElement serialize/deserialize signatures are all unchanged on disk — but the storage backend is different.  Bumping invalidates v28 caches so any old SD-side files (now reachable via LittleFS at the new prefix) get rejected and rebuilt.
constexpr uint16_t MAX_REASONABLE_PAGE_COUNT = 8192;

#ifndef SNAPIX_PERF_LOG
#define SNAPIX_PERF_LOG 0
#endif

inline uint32_t perfMsNow() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

inline void perfLog(const char* phase, uint32_t startedMs, const char* fmt = nullptr, ...) {
#if SNAPIX_PERF_LOG
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
// - bionicReading (1 byte)
// - fakeBold (1 byte)
// - viewportWidth (2 bytes)
// - viewportHeight (2 bytes)
// - pageCount (2 bytes)
// - isPartial (1 byte)
// - lutOffset (4 bytes)
constexpr uint32_t HEADER_SIZE = 1 + 4 + 4 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 2 + 1 + 4;

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

// v2.0.60: recursive LittleFS::mkdir.  Arduino LittleFS::mkdir is non-
// recursive — it fails if a parent doesn't exist.  Same helper as in
// Fb2.cpp; kept inline here so PageCache stays self-contained.
bool ensureCacheDir(const std::string& path) {
  if (path.empty() || path == "/") return true;
  if (LittleFS.exists(path.c_str())) return true;
  size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos && lastSlash > 0) {
    if (!ensureCacheDir(path.substr(0, lastSlash))) return false;
  }
  return LittleFS.mkdir(path.c_str());
}

bool evictCacheFile(const std::string& cachePath, const char* reason) {
  if (!LittleFS.exists(cachePath.c_str())) {
    return true;
  }

  if (LittleFS.remove(cachePath.c_str())) {
    LOG_INF(TAG, "Removed stale cache file reason=%s path=%s", reason ? reason : "unknown", cachePath.c_str());
    return true;
  }

  // LittleFS doesn't have SD's directory-cache flakiness, but if remove
  // genuinely failed (e.g. fs full / path locked), try renaming aside as
  // a last resort so the bad file doesn't keep getting re-loaded.
  const std::string quarantinePath = cachePath + ".stale";
  if (LittleFS.exists(quarantinePath.c_str())) {
    LittleFS.remove(quarantinePath.c_str());
  }
  if (LittleFS.rename(cachePath.c_str(), quarantinePath.c_str())) {
    LOG_INF(TAG, "Quarantined stale cache file reason=%s path=%s quarantine=%s", reason ? reason : "unknown",
            cachePath.c_str(), quarantinePath.c_str());
    return true;
  }

  LOG_ERR(TAG, "Failed to evict stale cache file reason=%s path=%s", reason ? reason : "unknown", cachePath.c_str());
  return false;
}

// v2.0.60: simplified close.  LittleFS lives on a separate SPI flash bus
// from SD/display, so SharedBusLock is no longer needed; flush() is the
// Arduino-File equivalent of SdFat's sync().
void closeFile(File& f) {
  if (f) {
    f.flush();
    f.close();
  }
}
}  // namespace

PageCache::PageCache(std::string cachePath) : cachePath_(std::move(cachePath)) {}

bool PageCache::ensureReadHandle() {
  if (readFile_) return true;
  readFile_ = LittleFS.open(cachePath_.c_str(), "r");
  if (readFile_) {
    readFileSize_ = readFile_.size();
    return true;
  }
  return false;
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

  if (residentPages_.capacity() < RESIDENT_PAGE_LIMIT) {
    residentPages_.reserve(RESIDENT_PAGE_LIMIT);
  }

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
  serialization::writePod(file_, config_.bionicReading);
  serialization::writePod(file_, config_.fakeBold);
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

  // v2.0.165 — fsync the write handle BEFORE any subsequent reader can
  // open a fresh read handle on the same path.  Without this, LittleFS
  // keeps the writes (LUT entries + updated header) in its internal
  // buffer; a concurrent read on another file handle sees the updated
  // header (lutOffset, pageCount) but the LUT bytes at that offset are
  // not yet committed to flash.  Visible as:
  //
  //   [ERR] [CACHE] [CACHE] LUT truncated at entry 0 of 24
  //                (lutOffset=275 file=371) path=.../sections/11.bin
  //
  // even though `file=371` matches what would be expected for 24
  // entries × 4 bytes after the offset.  The reader reads what's
  // on flash, not the writer's buffer.
  //
  // `flush()` forces the write through; once it returns, subsequent
  // LittleFS.open(path, "r") sees a consistent view.  Cost: one
  // flush per hot-extend (~5-10 ms typical), negligible vs the
  // millisecond-scale page-cache writes themselves.
  file_.flush();

  lutOffset_ = lutOffset;
  pageLut_ = lut;
  // Don't clear resident pages here — hot extend only appends pages,
  // existing offsets stay valid.  Callers that rebuild the file (cold
  // extend) already call clearResidentPages() explicitly.
  closeReadHandle();

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
  // v2.0.138 — default-init `pos` and use checked read.  Pre-fix, an
  // unchecked `readPod` left `pos` with whatever stack residue was
  // there if the LittleFS read came up short (e.g. truncated file,
  // concurrent write from BG worker mid-update).  That residue —
  // typically a heap/code pointer like 0x3C50C000 — was pushed into
  // pageLut_ and surfaced later as the "[CACHE] Invalid page position:
  // 1011724288 (file size: NNNN)" error, after which the cache had to
  // be cleared and rebuilt mid-session (cascading into the v2.0.136
  // parser-reset path and visible content discontinuity).
  readFile_.seek(lutOffset_);
  lut.clear();
  lut.reserve(pageCount_);
  for (uint16_t i = 0; i < pageCount_; i++) {
    uint32_t pos = 0;
    if (!serialization::readPodChecked(readFile_, pos)) {
      LOG_ERR(TAG, "[CACHE] LUT truncated at entry %u of %u (lutOffset=%u file=%zu) path=%s",
              static_cast<unsigned>(i), static_cast<unsigned>(pageCount_),
              static_cast<unsigned>(lutOffset_), fileSize, cachePath_.c_str());
      lut.clear();
      pageLut_.clear();
      closeReadHandle();
      return false;
    }
    lut.push_back(pos);
  }
  pageLut_ = lut;
  clearResidentPages();
  // v2.0.80: see comment in PageCache::load.  loadLut is called from extend
  // paths and from ensureLutLoaded; in both cases the LUT is now in memory
  // and follow-up reads (loadPage / prefetchWindow) re-open as needed.
  closeReadHandle();
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
  // v2.0.80: same rationale as PageCache::load — see comment there.  Header
  // data is now in members; don't pin the FD past the read.
  closeReadHandle();
  return true;
}

PageCache::ProbeResult PageCache::probe(const std::string& cachePath, const RenderConfig& config, bool cleanupInvalid) {
  ProbeResult result;
  if (!LittleFS.exists(cachePath.c_str())) {
    return result;
  }

  result.exists = true;

  File readFile = LittleFS.open(cachePath.c_str(), "r");
  if (!readFile) {
    return result;
  }

  const size_t fileSize = readFile.size();
  if (fileSize < HEADER_SIZE) {
    readFile.close();
    if (cleanupInvalid) {
      LittleFS.remove(cachePath.c_str());
    }
    return result;
  }

  uint8_t version;
  serialization::readPod(readFile, version);
  if (version != CACHE_FILE_VERSION) {
    readFile.close();
    if (cleanupInvalid) {
      LittleFS.remove(cachePath.c_str());
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
  serialization::readPod(readFile, fileConfig.bionicReading);
  serialization::readPod(readFile, fileConfig.fakeBold);
  serialization::readPod(readFile, fileConfig.viewportWidth);
  serialization::readPod(readFile, fileConfig.viewportHeight);
  if (config != fileConfig) {
    readFile.close();
    if (cleanupInvalid) {
      LittleFS.remove(cachePath.c_str());
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
      LittleFS.remove(cachePath.c_str());
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
  serialization::readPod(readFile_, fileConfig.bionicReading);
  serialization::readPod(readFile_, fileConfig.fakeBold);
  serialization::readPod(readFile_, fileConfig.viewportWidth);
  serialization::readPod(readFile_, fileConfig.viewportHeight);

  if (config != fileConfig) {
    closeReadHandle();
    LOG_INF(TAG, "Config mismatch, invalidating cache — font:%d/%d lc:%d/%d il:%d/%d sl:%d/%d pa:%d/%d hy:%d/%d si:%d/%d br:%d/%d fb:%d/%d vw:%d/%d vh:%d/%d",
            config.fontId, fileConfig.fontId, config.lineCompression, fileConfig.lineCompression,
            config.indentLevel, fileConfig.indentLevel, config.spacingLevel, fileConfig.spacingLevel,
            config.paragraphAlignment, fileConfig.paragraphAlignment, config.hyphenation, fileConfig.hyphenation,
            config.showImages, fileConfig.showImages, config.bionicReading, fileConfig.bionicReading,
            config.fakeBold, fileConfig.fakeBold, config.viewportWidth, fileConfig.viewportWidth,
            config.viewportHeight, fileConfig.viewportHeight);
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
  // v2.0.80: close readFile_ now that the header is parsed into members.
  // Pre-2.0.80 the handle persisted until destructor / explicit clear, which
  // caused two-instance bin races: a TOC-jump handler loads spine N from the
  // main thread, then the TOC worker creates its own PageCache for the same
  // spine and tries to clear() / evict the bin file.  LittleFS refused with
  // "Has open FD" because the main thread's readFile_ was still open.  All
  // subsequent loadPage / prefetchWindow calls re-open via ensureReadHandle()
  // anyway, so closing here just shifts cost (one open per page-read burst,
  // matching the original SD-era behaviour).
  closeReadHandle();
  LOG_INF(TAG, "Loaded: %d pages, partial=%d", pageCount_, isPartial_);
  return true;
}

bool PageCache::create(ContentParser& parser, const RenderConfig& config, uint16_t maxPages, uint16_t skipPages,
                       const AbortCallback& shouldAbort) {
  LOG_INF(TAG, "[CACHE] create start path=%s maxPages=%u skipPages=%u", cachePath_.c_str(),
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
    file_ = LittleFS.open(cachePath_.c_str(), "r+");
    if (!file_) {
      LOG_ERR(TAG, "Failed to open cache file for append");
      return false;
    }
    file_.seek(file_.size());  // Append after old LUT
  } else {
    // Fresh create
    closeReadHandle();
    closeFile(file_);

    // Ensure full directory hierarchy exists.  LittleFS.mkdir is non-recursive,
    // so walk up the path creating each ancestor.  Much simpler than the SD
    // hierarchy dance — LittleFS doesn't have SdFat's directory-cache flakiness.
    {
      const auto slash = cachePath_.rfind('/');
      if (slash != std::string::npos) {
        const std::string parentDir = cachePath_.substr(0, slash);
        if (!ensureCacheDir(parentDir)) {
          LOG_ERR(TAG, "Failed to create parent dir: %s", parentDir.c_str());
        }
      }
    }

    file_ = LittleFS.open(cachePath_.c_str(), "w");
    if (!file_) {
      if (LittleFS.exists(cachePath_.c_str())) {
        LOG_ERR(TAG, "Retrying cache create after removing stale file: %s", cachePath_.c_str());
        evictCacheFile(cachePath_, "fresh-create-open-failed");
        file_ = LittleFS.open(cachePath_.c_str(), "w");
        if (!file_) {
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
    closeFile(file_);
    // Fresh create writes a placeholder header before parsing starts. If we abort
    // here and keep that file, later probes see a zero-page cache and spin on
    // "Rejecting empty/incomplete cache file" for the same section.
    if (!isExtendPass) {
      LittleFS.remove(cachePath_.c_str());
    }
    LOG_INF(TAG, "Aborted before parsing");
    return false;
  }

  uint16_t parsedPages = 0;
  bool hitMaxPages = false;
  bool aborted = false;

  bool success = false;
  try {
    success = parser.parsePages(
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
  } catch (const std::bad_alloc&) {
    LOG_ERR(TAG, "OOM during create (pages=%u)", static_cast<unsigned>(pageCount_));
    // Treat like an abort — continue with whatever pages were serialized.
    // Mark partial so the next extend attempt can pick up where we left off.
    aborted = true;
    parser.reset();
  }

  // Check if we were aborted
  if (shouldAbort && shouldAbort()) {
    aborted = true;
    LOG_INF(TAG, "Aborted during parsing");
  }

  const bool madeForwardProgress = pageCount_ > initialPageCount;

  if (!success && pageCount_ == 0) {
    closeFile(file_);
    // Remove file to prevent corrupt/incomplete cache
    evictCacheFile(cachePath_, "create-empty-failure");
    LOG_ERR(TAG, "[CACHE] create failed/aborted path=%s pages=%u success=%u aborted=%u", cachePath_.c_str(),
            static_cast<unsigned>(pageCount_), static_cast<unsigned>(success), static_cast<unsigned>(aborted));
    return false;
  }

  if (aborted || !success) {
    if (!madeForwardProgress) {
      closeFile(file_);

      // During extend/rebuild passes the old header/LUT still points at the last
      // known-good cache contents. Keep that file instead of deleting the whole
      // section cache when the new pass gets aborted.
      if (isExtendPass && initialPageCount > 0) {
        pageCount_ = initialPageCount;
        isPartial_ = true;
        pageLut_ = lut;
        clearResidentPages();
        LOG_INF(TAG,
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
      closeFile(file_);
      // writeLut failed BEFORE updating the on-disk header — the file (if kept)
      // still references the previous LUT and pre-extend page count.  Roll the
      // in-memory state back to match what's actually on disk; otherwise
      // pageCount_ overshoots pageLut_ and a subsequent loadPage(N) reads
      // pageLut_[N] out of bounds.
      if (!isExtendPass) {
        evictCacheFile(cachePath_, "create-partial-write-lut-failed");
        pageCount_ = 0;
        isPartial_ = false;
        pageLut_.clear();
        clearResidentPages();
      } else {
        pageCount_ = initialPageCount;
        // pageLut_ already holds the pre-extend snapshot from loadLut().
        // isPartial_ stays true — that's the on-disk state we're preserving.
      }
      return false;
    }

    file_.flush();
    closeFile(file_);
    LOG_INF(TAG, "[CACHE] create partial path=%s pages=%u success=%u aborted=%u", cachePath_.c_str(),
            static_cast<unsigned>(pageCount_), static_cast<unsigned>(success), static_cast<unsigned>(aborted));
    perfLog("cache-create", startMs, "(pages=%u partial=%u)", pageCount_, static_cast<unsigned>(isPartial_));
    return true;
  }

  isPartial_ = parser.hasMoreContent();
  if (!madeForwardProgress && isPartial_) {
    closeFile(file_);
    if (isExtendPass && initialPageCount > 0) {
      pageCount_ = initialPageCount;
      pageLut_ = lut;
      clearResidentPages();
      LOG_INF(TAG, "[CACHE] create kept previous cache path=%s pages=%u reason=no-progress-partial",
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
    closeFile(file_);
    evictCacheFile(cachePath_, "create-write-lut-failed");
    return false;
  }

  file_.flush();
  closeFile(file_);
  LOG_INF(TAG, "[CACHE] create done path=%s pages=%u partial=%u", cachePath_.c_str(),
          static_cast<unsigned>(pageCount_), static_cast<unsigned>(isPartial_));
  perfLog("cache-create", startMs, "(pages=%u partial=%u)", pageCount_, static_cast<unsigned>(isPartial_));
  return true;
}

bool PageCache::extend(ContentParser& parser, uint16_t additionalPages, const AbortCallback& shouldAbort) {
  LOG_INF(TAG, "[CACHE] extend start path=%s current=%u add=%u partial=%u canResume=%u", cachePath_.c_str(),
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
  // v2.0.114: non-const so the cold path can shrink to chunk=1 if heap is
  // tight — see the chunk=1 fallback below.
  uint16_t chunk =
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
    file_ = LittleFS.open(cachePath_.c_str(), "r+");
    if (!file_) {
      LOG_ERR(TAG, "Failed to open cache file for hot extend");
      return false;
    }
    file_.seek(file_.size());

    const uint16_t pagesBefore = pageCount_;
    bool parseOk = false;
    try {
      parseOk = parser.parsePages(
          [this, &lut](std::unique_ptr<Page> page) {
            const uint32_t position = file_.position();
            if (!page->serialize(file_)) return;
            lut.push_back(position);
            pageCount_++;
          },
          chunk, shouldAbort);
    } catch (const std::bad_alloc&) {
      LOG_ERR(TAG, "OOM during hot extend (pages %u→%u)", static_cast<unsigned>(pagesBefore),
              static_cast<unsigned>(pageCount_));
      // Align pageCount with lut — only entries present in both are addressable.
      pageCount_ = static_cast<uint16_t>(lut.size());
      // Parser state is undefined after an exception; mark partial and
      // invalidate the parser so the next attempt uses cold extend.
      isPartial_ = true;
      parser.reset();
      // Save whatever progress was made.  If writeLut() fails, roll back
      // in-memory state to match what's actually on disk — otherwise
      // the next loadPage(N) reads pageLut_[N] out-of-bounds.
      if (pageCount_ > pagesBefore) {
        if (!writeLut(lut)) {
          LOG_ERR(TAG, "writeLut failed in OOM recovery — rolling back in-memory state");
          pageCount_ = pagesBefore;
          closeFile(file_);
          return false;
        }
      }
      closeFile(file_);
      return pageCount_ > pagesBefore;
    }

    if (!parseOk && pageCount_ == pagesBefore) {
      closeFile(file_);
      LOG_ERR(TAG, "[CACHE] hot extend failed path=%s pagesBefore=%u", cachePath_.c_str(),
              static_cast<unsigned>(pagesBefore));
      return false;
    }

    isPartial_ = parser.hasMoreContent();

    if (pageCount_ == pagesBefore) {
      if (!isPartial_) {
        if (!writeLut(lut)) {
          closeFile(file_);
          LittleFS.remove(cachePath_.c_str());
          return false;
        }
        file_.flush();
        closeFile(file_);
        LOG_INF(TAG, "[CACHE] hot extend reached end without new pages path=%s pages=%u", cachePath_.c_str(),
                static_cast<unsigned>(pageCount_));
        return true;
      }

      closeFile(file_);
      parser.reset();
      LOG_INF(TAG, "[CACHE] hot extend stalled without page growth path=%s pages=%u", cachePath_.c_str(),
              static_cast<unsigned>(pageCount_));
      return false;
    }

    if (!writeLut(lut)) {
      closeFile(file_);
      LittleFS.remove(cachePath_.c_str());
      return false;
    }

    file_.flush();
    closeFile(file_);
    LOG_INF(TAG, "[CACHE] hot extend done path=%s pages=%u partial=%u", cachePath_.c_str(),
            static_cast<unsigned>(pageCount_), static_cast<unsigned>(isPartial_));
    LOG_INF(TAG, "Hot extend done: %d pages, partial=%d", pageCount_, isPartial_);

    // Repeated hot extends keep the parser's XML state (and its internal
    // allocations) pinned in the middle of the heap.  Transient page/layout
    // objects allocated and freed around it fragment the heap over time.
    // When the largest contiguous block drops below a safe threshold, reset
    // the parser so the *next* extend uses the cold path, which re-parses
    // from scratch in a fresh allocation context and allows the allocator
    // to coalesce freed blocks.  The cost is one cold rebuild (~0.5-1 s)
    // every 8-12 hot extends — far better than an OOM crash.
    if (isPartial_) {
      const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      if (largestBlock < 15 * 1024) {
        LOG_INF(TAG, "Resetting parser to defragment heap (largest=%u free=%u)",
                static_cast<unsigned>(largestBlock),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
        parser.reset();
      }
    }

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
  // v2.0.114: non-const so the chunk=1 fallback below can update it after
  // shrinking `chunk`.
  uint16_t targetPages = pageCount_ + chunk;

  // v2.0.64: cold rebuild parses content from byte 0 with a fresh parser
  // session.  That session allocates significantly more transient memory
  // than a hot extend (whole-document anchor map, makePages buffers,
  // freshly-built TextBlock pools per page).  The minimum-safe headroom
  // we've measured for "Атомные привычки" pages is ~25 KB largest /
  // ~50 KB total free — below that, mid-parse `make_shared<TextBlock>`
  // hits a fragmented heap, throws std::bad_alloc, and the runtime can't
  // even allocate the exception object → __terminate → abort() (see
  // multi_heap_malloc:267 stack from v2.0.63 crash).
  //
  // The worker startup gate (isHeapCritical: free<28K || largest<10K)
  // is too lenient for cold rebuild because cumulative alloc during
  // parse exceeds that headroom.  Refuse to rebuild when heap is below
  // the cold-rebuild-specific threshold; the caller treats this as a
  // transient failure and BG cache stays partial until heap recovers
  // (memory trim, parser reset, page eviction).
  // A one-page foreground fill has a much smaller transient target than the
  // multi-page background rebuild; keep the strict gate for background work.
  // v2.0.114: the per-chunk threshold table.  Multi-page chunks need
  // ~25 KB largest contiguous to be safe (the ~6 KB Calibre word
  // anchor map, ~10 KB makePages buffers, ~4-10 KB transient TextBlock
  // pools per page accumulate).  Single-page rebuilds need only ~8 KB
  // because nothing accumulates beyond one page's worth of state.
  // The threshold pair is recomputed if we shrink `chunk` mid-flight
  // below (chunk=1 fallback path).
  auto coldRebuildThresholds = [](uint16_t c) -> std::pair<size_t, size_t> {
    return (c <= 1) ? std::make_pair(static_cast<size_t>(8 * 1024), static_cast<size_t>(24 * 1024))
                    : std::make_pair(static_cast<size_t>(25 * 1024), static_cast<size_t>(50 * 1024));
  };
  auto thresh = coldRebuildThresholds(chunk);
  size_t kColdRebuildMinLargest = thresh.first;
  size_t kColdRebuildMinFree = thresh.second;
  size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (largestBlock < kColdRebuildMinLargest || freeBytes < kColdRebuildMinFree) {
    // v2.0.81: try one heap-defrag pass before giving up.  The JPEGDEC
    // instance (~25 KB) and decode arena (~7-20 KB) live mid-heap from the
    // first image decode forward; they pin `largest` below the cold-rebuild
    // threshold even when total free is plentiful.  Releasing them returns
    // ~30-45 KB to one contiguous block.  Next image decode re-allocates
    // them at the end of the heap (~10 ms cost) — far cheaper than the
    // alternative: a permanently stuck cache extender that freezes the
    // reader at the last cacheable page.
    LOG_INF(TAG,
            "[CACHE] cold extend pre-flight tight (free=%u largest=%u) — releasing JPEG persistent state",
            static_cast<unsigned>(freeBytes), static_cast<unsigned>(largestBlock));
    JpegToBmpConverter::releaseAllPersistent();
    freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largestBlock < kColdRebuildMinLargest || freeBytes < kColdRebuildMinFree) {
      // v2.0.114 — chunk=1 fallback before deferring.
      //
      // The empirical 25 KB largest / 50 KB free threshold for chunk>1
      // accumulates from per-page transient allocations during a multi-page
      // rebuild.  But a SINGLE-page rebuild only needs ~8 KB largest /
      // ~24 KB free (one TextBlock pool, one wordWidths vector, one
      // ParsedText spill, then released).  If multi-page is blocked but
      // single-page would fit, shrink the chunk and proceed — the user
      // sees ONE new page per extend call instead of nothing at all.
      // Subsequent extend calls retry the full chunk size; once the heap
      // recovers (parser reset, image decode flush) chunk>1 starts
      // succeeding again.
      if (chunk > 1) {
        auto thresh1 = coldRebuildThresholds(1);
        const size_t minLargest1 = thresh1.first;
        const size_t minFree1 = thresh1.second;
        if (largestBlock >= minLargest1 && freeBytes >= minFree1) {
          LOG_INF(TAG,
                  "[CACHE] cold extend chunk=1 fallback (free=%u largest=%u below %u/%u for chunk=%u, "
                  "fits %u/%u for chunk=1)",
                  static_cast<unsigned>(freeBytes), static_cast<unsigned>(largestBlock),
                  static_cast<unsigned>(kColdRebuildMinFree),
                  static_cast<unsigned>(kColdRebuildMinLargest),
                  static_cast<unsigned>(chunk),
                  static_cast<unsigned>(minFree1), static_cast<unsigned>(minLargest1));
          chunk = 1;
          targetPages = pageCount_ + 1;  // shrink rebuild scope to match
          kColdRebuildMinLargest = minLargest1;
          kColdRebuildMinFree = minFree1;
          // Fall through — pre-flight now passes at chunk=1 thresholds.
        } else {
          LOG_INF(TAG,
                  "[CACHE] cold extend deferred — heap headroom insufficient even for chunk=1 "
                  "(free=%u/%u largest=%u/%u)",
                  static_cast<unsigned>(freeBytes), static_cast<unsigned>(minFree1),
                  static_cast<unsigned>(largestBlock), static_cast<unsigned>(minLargest1));
          return false;
        }
      } else {
        LOG_INF(TAG,
                "[CACHE] cold extend deferred — heap headroom still insufficient after JPEG release "
                "(free=%u/%u largest=%u/%u)",
                static_cast<unsigned>(freeBytes), static_cast<unsigned>(kColdRebuildMinFree),
                static_cast<unsigned>(largestBlock), static_cast<unsigned>(kColdRebuildMinLargest));
        return false;
      }
    } else {
      LOG_INF(TAG, "[CACHE] cold extend headroom recovered after JPEG release (free=%u largest=%u)",
              static_cast<unsigned>(freeBytes), static_cast<unsigned>(largestBlock));
    }
  }
  LOG_INF(TAG, "Cold extend from %d to %d pages (free=%u largest=%u)", currentPages, targetPages,
          static_cast<unsigned>(freeBytes), static_cast<unsigned>(largestBlock));

  // v2.0.110 (audit fix #4 — cold-rebuild trap elimination):
  //
  // Pre-fix: `parser.reset()` was called HERE before the rebuild started,
  // wiping any in-progress parser state.  The intent was to force a clean
  // re-parse from byte 0 for "stable pagination" (so newly-emitted pages
  // wouldn't mix with the kept old cache).  Side effect: every cold
  // rebuild followed by partial-success-or-preempt left the parser in a
  // reset state, so the NEXT extend attempt re-entered cold path again.
  // That's the "cold rebuild trap" — Agent #2 audit root cause of
  // "buttons frozen for 25-75 s, then fallback to old page" symptom.
  //
  // The reset is unnecessary: `rebuiltCache.create(parser, ...)` already
  // calls `parser.parsePages(...)` which initializes the parser from
  // scratch internally when `initialized_ == false` OR when
  // `canResume() == false`.  If the parser comes in WITH a suspended
  // resumable state (from a previous hot extend or cooperative abort),
  // `parsePages` resumes from byte cursor — saving the cold-rebuild
  // penalty entirely.  Either path produces consistent pages in the
  // sidecar .rebuild file; the kept old cache is untouched.
  const std::string rebuildPath = cachePath_ + ".rebuild";
  if (LittleFS.exists(rebuildPath.c_str())) {
    LittleFS.remove(rebuildPath.c_str());
  }

  PageCache rebuiltCache(rebuildPath);
  const bool rebuildResult = rebuiltCache.create(parser, config_, targetPages, 0, shouldAbort);
  const bool canPromoteRebuild = rebuildResult && rebuiltCache.pageCount() >= currentPages;

  if (!canPromoteRebuild) {
    // v2.0.110: do NOT reset parser here either.  Pre-fix this wiped
    // suspended state from the partial rebuild, guaranteeing the next
    // extend also went cold.  Now we KEEP whatever cooperative-suspend
    // state the parser holds — next extend can resume from where this
    // rebuild left off, going hot instead of cold.  The pagination-
    // consistency concern is moot here because the sidecar .rebuild
    // file is DISCARDED (line below) and the kept old cache is the
    // authoritative source.  The parser's byte cursor advanced into
    // the chapter source during rebuild, so a future hot extend from
    // that cursor produces pages consistent with what it would produce
    // if cold-rebuilt from the same cursor.
    if (LittleFS.exists(rebuildPath.c_str())) {
      LittleFS.remove(rebuildPath.c_str());
    }
    LOG_INF(TAG, "[CACHE] cold extend kept previous path=%s rebuilt=%u rebuiltPages=%u current=%u canResumeNext=%u",
            cachePath_.c_str(), static_cast<unsigned>(rebuildResult), static_cast<unsigned>(rebuiltCache.pageCount()),
            static_cast<unsigned>(currentPages), static_cast<unsigned>(parser.canResume()));
    // Return false to break the caller's retry loop — the rebuild couldn't
    // produce enough pages and retrying with the same memory won't help.
    return false;
  }

  // ── Promote the rebuild file to replace the original ──────────────
  // v2.0.60: LittleFS lives on a separate SPI bus, no SharedBusLock dance
  // needed.  Same backup/promote/rollback flow as before.
  bool result = false;
  {
    closeReadHandle();
    closeFile(file_);
    clearResidentPages();
    pageLut_.clear();

    const std::string backupPath = cachePath_ + ".bak";
    if (LittleFS.exists(backupPath.c_str())) {
      LittleFS.remove(backupPath.c_str());
    }

    const bool hadOldCache = LittleFS.exists(cachePath_.c_str());
    if (hadOldCache && !LittleFS.rename(cachePath_.c_str(), backupPath.c_str())) {
      LOG_ERR(TAG, "Failed to back up old cache before cold promote: %s", cachePath_.c_str());
      LittleFS.remove(rebuildPath.c_str());
      return false;
    }

    if (!LittleFS.rename(rebuildPath.c_str(), cachePath_.c_str())) {
      LOG_ERR(TAG, "Failed to promote rebuilt cache: %s", cachePath_.c_str());
      LittleFS.remove(rebuildPath.c_str());
      if (hadOldCache) {
        LittleFS.rename(backupPath.c_str(), cachePath_.c_str());
      }
      return false;
    }

    result = load(config_);
    if (!result) {
      LOG_ERR(TAG, "Failed to reload promoted cache: %s", cachePath_.c_str());
      LittleFS.remove(cachePath_.c_str());
      if (hadOldCache) {
        LittleFS.rename(backupPath.c_str(), cachePath_.c_str());
        if (load(config_)) {
          return true;
        }
      }
      return false;
    }

    if (hadOldCache && LittleFS.exists(backupPath.c_str())) {
      LittleFS.remove(backupPath.c_str());
    }
  }

  // No forward progress — either content is truly finished or OOM/abort
  // prevented creating more pages.
  if (result && pageCount_ <= currentPages) {
    if (!parser.hasMoreContent()) {
      // Parser has no more content → section is truly complete.
      LOG_INF(TAG, "No progress during extend (%d pages), marking complete", pageCount_);
      isPartial_ = false;
    } else {
      // Parser has more content but OOM/abort prevented creating pages.
      // Return false to break the caller's retry loop — retrying won't help
      // since memory conditions haven't changed.
      LOG_INF(TAG, "[CACHE] cold extend stalled at %u pages (OOM/abort), signaling no progress",
              static_cast<unsigned>(pageCount_));
      return false;
    }
  }

  LOG_INF(TAG, "[CACHE] cold extend done path=%s result=%u pages=%u partial=%u", cachePath_.c_str(),
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

  if (!ensureReadHandle()) return nullptr;
  const size_t fileSize = readFileSize_;
  const uint32_t pagePos = pageLut_[pageNum];

  // Validate page position
  if (pagePos < HEADER_SIZE || pagePos >= fileSize) {
    LOG_ERR(TAG, "Invalid page position: %u (file size: %zu)", pagePos, fileSize);
    closeReadHandle();
    return nullptr;
  }

  // Read page
  readFile_.seek(pagePos);
  auto page = Page::deserialize(readFile_);

  // v2.0.80: release the FD as soon as the deserialize finishes.  Pre-2.0.80
  // the handle persisted across page-turn boundaries, blocking eviction /
  // rename when another task (e.g. TOC-jump worker) needed to mutate the
  // same path.  Keeping it open was a micro-optimisation for consecutive
  // page-reads; the resident-page cache already covers that case, so
  // closing here only costs an extra LittleFS.open on a true cache miss.
  closeReadHandle();

  if (page) {
    auto sharedPage = std::shared_ptr<Page>(std::move(page));
    putResidentPage(pageNum, sharedPage);
    perfLog("page-load", startMs, "(page=%u source=sd resident=%zu)", pageNum, residentPages_.size());
    return sharedPage;
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
  // v2.0.80: release the FD now that the prefetch burst is complete.  See
  // comment in loadPage() — keeping it open across navigation boundaries
  // caused the eviction-on-TOC-jump bug.
  closeReadHandle();
  perfLog("page-prefetch", startMs, "(center=%u dir=%d resident=%zu)", centerPage, direction, residentPages_.size());
}

bool PageCache::clear() {
  clearResidentPages();
  pageLut_.clear();
  closeReadHandle();
  closeFile(file_);
  pageCount_ = 0;
  isPartial_ = false;
  lutOffset_ = 0;
  readFileSize_ = 0;
  if (!LittleFS.exists(cachePath_.c_str())) {
    return true;
  }
  return evictCacheFile(cachePath_, "clear");
}
