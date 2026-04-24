#include "EpubChapterParser.h"

#include <Arduino.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <GfxRenderer.h>
#include <Html5Normalizer.h>
#include <Hyphenation.h>
#include <Logging.h>
#include <Page.h>
#include <SDCardManager.h>
#include <core/CrashDebug.h>
#include <esp_heap_caps.h>

#define TAG "EPUB_CHAP"

#include <utility>

namespace {
constexpr uint32_t PREPARE_TIMEOUT_MS = 20000;
constexpr size_t PREPARE_MIN_FREE_HEAP = 8192;

inline std::string epubPreparedSourcePath(const std::string& epubCachePath, int spineIndex) {
  return epubCachePath + "/chapters/" + std::to_string(spineIndex) + ".src.html";
}

inline std::string epubPreparedNormalizedPath(const std::string& epubCachePath, int spineIndex) {
  return epubCachePath + "/chapters/" + std::to_string(spineIndex) + ".norm.html";
}

inline std::string epubPreparedWorkPath(const std::string& basePath) { return basePath + ".work"; }

bool isReadableNonEmptyFile(const std::string& path) {
  FsFile file;
  if (!SdMan.openFileForRead("EPUB", path, file)) {
    return false;
  }
  const bool ok = file.size() > 0;
  file.close();
  return ok;
}

bool looksLikePreparedHtmlFile(const std::string& path) {
  FsFile file;
  if (!SdMan.openFileForRead("EPUB", path, file)) {
    return false;
  }

  uint8_t prefix[128] = {};
  const int bytesRead = file.read(prefix, sizeof(prefix));
  file.close();
  if (bytesRead <= 0) {
    return false;
  }

  int i = 0;
  if (bytesRead >= 3 && prefix[0] == 0xEF && prefix[1] == 0xBB && prefix[2] == 0xBF) {
    i = 3;
  }

  for (; i < bytesRead; ++i) {
    const uint8_t c = prefix[i];
    if (c == 0) {
      return false;
    }
    if (c == '<') {
      return true;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f') {
      continue;
    }
    if (c < 0x20) {
      return false;
    }
    // Prepared chapter caches should start with markup, not arbitrary payload.
    return false;
  }

  return false;
}

bool evictPreparedHtmlFile(const std::string& path, const char* label, const char* reason) {
  if (path.empty() || !SdMan.exists(path.c_str())) {
    return true;
  }

  const char* why = reason ? reason : "unknown";
  if (SdMan.remove(path.c_str())) {
    LOG_INF(TAG, "[CONTENT][EPUB] dropped stale %s cache reason=%s path=%s", label, why, path.c_str());
    return true;
  }

  const std::string quarantinePath = path + ".stale";
  if (SdMan.exists(quarantinePath.c_str())) {
    SdMan.remove(quarantinePath.c_str());
  }

  if (SdMan.rename(path.c_str(), quarantinePath.c_str())) {
    LOG_INF(TAG, "[CONTENT][EPUB] quarantined stale %s cache reason=%s path=%s quarantine=%s", label, why,
            path.c_str(), quarantinePath.c_str());
    return true;
  }

  LOG_ERR(TAG, "[CONTENT][EPUB] failed to drop stale %s cache reason=%s path=%s", label, why, path.c_str());
  return false;
}

bool shouldAbortPreparePhase(const AbortCallback& externalAbort, const uint32_t startedMs) {
  if (externalAbort && externalAbort()) {
    return true;
  }

  if (millis() - startedMs > PREPARE_TIMEOUT_MS) {
    LOG_ERR(TAG, "EPUB prepare timeout exceeded (%lu ms)", static_cast<unsigned long>(PREPARE_TIMEOUT_MS));
    return true;
  }

  const size_t freeHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeHeap < PREPARE_MIN_FREE_HEAP) {
    LOG_ERR(TAG, "Low memory during EPUB prepare (%zu bytes free)", freeHeap);
    return true;
  }

  return false;
}

}  // namespace

EpubChapterParser::EpubChapterParser(std::shared_ptr<Epub> epub, int spineIndex, GfxRenderer& renderer,
                                     const RenderConfig& config, const std::string& imageCachePath,
                                     const bool quickImageDecode)
    : epub_(std::move(epub)),
      spineIndex_(spineIndex),
      renderer_(renderer),
      config_(config),
      imageCachePath_(imageCachePath),
      quickImageDecode_(quickImageDecode) {}

EpubChapterParser::~EpubChapterParser() {
  liveParser_.reset();
  releasePreparedPaths();
}

void EpubChapterParser::releasePreparedPaths() {
  sourceHtmlPath_.clear();
  normalizedPath_.clear();
  parseHtmlPath_.clear();
}

void EpubChapterParser::invalidatePreparedHtmlCaches(const char* reason) {
  const char* why = reason ? reason : "unknown";
  evictPreparedHtmlFile(normalizedPath_, "normalized", why);
  evictPreparedHtmlFile(sourceHtmlPath_, "source", why);
}

void EpubChapterParser::reset() {
  liveParser_.reset();
  releasePreparedPaths();
  initialized_ = false;
  hasMore_ = true;
  chapterBasePath_.clear();
  anchorMap_.clear();
}

bool EpubChapterParser::canResume() const {
  // Resume whenever the live parser was cooperatively suspended and its state is
  // still coherent. Fatal parser aborts remain non-resumable and force a restart.
  return initialized_ && liveParser_ != nullptr && liveParser_->isSuspended() && !liveParser_->wasAborted();
}

const std::vector<std::pair<std::string, uint16_t>>& EpubChapterParser::getAnchorMap() const {
  if (liveParser_) {
    return liveParser_->getAnchorMap();
  }
  return anchorMap_;
}

bool EpubChapterParser::prepareChapterHtml(const AbortCallback& shouldAbort) {
  LOG_INF(TAG, "[CONTENT][EPUB] prepare start spine=%d", spineIndex_);
  const auto localPath = epub_->getSpineItem(spineIndex_).href;
  const std::string canonicalSourcePath = epubPreparedSourcePath(epub_->getCachePath(), spineIndex_);
  const std::string canonicalNormalizedPath = epubPreparedNormalizedPath(epub_->getCachePath(), spineIndex_);
  sourceHtmlPath_ = canonicalSourcePath;
  normalizedPath_ = canonicalNormalizedPath;
  parseHtmlPath_.clear();
  const uint32_t prepareStartedMs = millis();
  auto shouldAbortPrepare = [&]() -> bool { return shouldAbortPreparePhase(shouldAbort, prepareStartedMs); };

  if (isReadableNonEmptyFile(normalizedPath_) && looksLikePreparedHtmlFile(normalizedPath_)) {
    parseHtmlPath_ = normalizedPath_;
    LOG_INF(TAG, "[CONTENT][EPUB] prepare normalized cache hit spine=%d", spineIndex_);
    return true;
  } else if (SdMan.exists(normalizedPath_.c_str())) {
    LOG_INF(TAG, "[CONTENT][EPUB] prepare drop invalid normalized cache spine=%d path=%s", spineIndex_,
            normalizedPath_.c_str());
    if (!evictPreparedHtmlFile(normalizedPath_, "normalized", "prepare-invalid-normalized")) {
      normalizedPath_ = epubPreparedWorkPath(canonicalNormalizedPath);
      evictPreparedHtmlFile(normalizedPath_, "normalized-work", "prepare-reset-work-normalized");
      LOG_INF(TAG, "[CONTENT][EPUB] prepare switched normalized cache path spine=%d path=%s", spineIndex_,
              normalizedPath_.c_str());
    }
  }

  if (SdMan.exists(sourceHtmlPath_.c_str()) && !looksLikePreparedHtmlFile(sourceHtmlPath_)) {
    LOG_INF(TAG, "[CONTENT][EPUB] prepare drop invalid source cache spine=%d path=%s", spineIndex_,
            sourceHtmlPath_.c_str());
    if (!evictPreparedHtmlFile(sourceHtmlPath_, "source", "prepare-invalid-source")) {
      sourceHtmlPath_ = epubPreparedWorkPath(canonicalSourcePath);
      normalizedPath_ = epubPreparedWorkPath(canonicalNormalizedPath);
      evictPreparedHtmlFile(sourceHtmlPath_, "source-work", "prepare-reset-work-source");
      evictPreparedHtmlFile(normalizedPath_, "normalized-work", "prepare-reset-work-normalized");
      LOG_INF(TAG, "[CONTENT][EPUB] prepare switched source cache path spine=%d path=%s", spineIndex_,
              sourceHtmlPath_.c_str());
    }
  }

  if (!isReadableNonEmptyFile(sourceHtmlPath_)) {
    bool success = false;
    for (int attempt = 0; attempt < 3 && !success; attempt++) {
      if (attempt > 0) {
        LOG_INF(TAG, "Retrying chapter extraction (attempt %d)...", attempt + 1);
        delay(50);
      }

      if (SdMan.exists(sourceHtmlPath_.c_str())) {
        SdMan.remove(sourceHtmlPath_.c_str());
      }

      FsFile sourceHtml;
      if (!SdMan.openFileForWrite("EPUB", sourceHtmlPath_, sourceHtml)) {
        continue;
      }

      // Reuse frame buffer (48KB) as ZIP decompression dict (32KB) — safe because
      // the background task owns the renderer and display isn't active during parsing
      snapix::crashdebug::mark(snapix::crashdebug::CrashPhase::EpubTocExtract, static_cast<int16_t>(spineIndex_));
      success = epub_->readItemContentsToStream(localPath, sourceHtml, 1024, renderer_.getFrameBuffer(),
                                                shouldAbortPrepare);
      sourceHtml.close();

      if (!success && SdMan.exists(sourceHtmlPath_.c_str())) {
        SdMan.remove(sourceHtmlPath_.c_str());
      }
    }

    if (!success) {
      LOG_ERR(TAG, "[CONTENT][EPUB] prepare extract failed spine=%d", spineIndex_);
      return false;
    }
  } else {
    LOG_INF(TAG, "[CONTENT][EPUB] prepare source cache hit spine=%d", spineIndex_);
  }

  parseHtmlPath_ = sourceHtmlPath_;
  snapix::crashdebug::mark(snapix::crashdebug::CrashPhase::EpubTocNormalize, static_cast<int16_t>(spineIndex_));
  if (html5::normalizeVoidElements(sourceHtmlPath_, normalizedPath_, shouldAbortPrepare)) {
    parseHtmlPath_ = normalizedPath_;
    LOG_INF(TAG, "[CONTENT][EPUB] prepare normalized spine=%d", spineIndex_);
  } else if (isReadableNonEmptyFile(normalizedPath_)) {
    parseHtmlPath_ = normalizedPath_;
    LOG_INF(TAG, "[CONTENT][EPUB] prepare normalized fallback hit spine=%d", spineIndex_);
  } else {
    LOG_INF(TAG, "[CONTENT][EPUB] prepare parse source without normalization spine=%d", spineIndex_);
  }

  LOG_INF(TAG, "[CONTENT][EPUB] prepare done spine=%d path=%s", spineIndex_, parseHtmlPath_.c_str());
  return !parseHtmlPath_.empty();
}

bool EpubChapterParser::parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages,
                                   const AbortCallback& shouldAbort) {
  LOG_INF(TAG, "[CONTENT][EPUB] parsePages start spine=%d initialized=%u suspended=%u aborted=%u resumable=%u maxPages=%u",
          spineIndex_, static_cast<unsigned>(initialized_),
          static_cast<unsigned>(liveParser_ ? liveParser_->isSuspended() : 0),
          static_cast<unsigned>(liveParser_ ? liveParser_->wasAborted() : 0), static_cast<unsigned>(canResume()),
          static_cast<unsigned>(maxPages));
  // RESUME PATH: parser is alive from a previous call, just resume.
  // The liveParser_'s completePageFn captures `this` and delegates to member state
  // (onPageComplete_, maxPages_, etc.), so we just update those for the new batch.
  if (canResume()) {
    Hyphenation::setLanguage(epub_->getLanguage());
    onPageComplete_ = onPageComplete;
    maxPages_ = maxPages;
    pagesCreated_ = 0;
    hitMaxPages_ = false;

    bool success = liveParser_->resumeParsing();
    const bool parserSuspended = liveParser_->isSuspended();
    const bool parserAborted = liveParser_->wasAborted();
    const bool noProgressRetryable = pagesCreated_ == 0 && (parserSuspended || parserAborted);
    if (noProgressRetryable) {
      success = false;
    }

    hasMore_ = parserSuspended || parserAborted || (!success && pagesCreated_ > 0);

    LOG_INF(TAG, "[CONTENT][EPUB] parsePages resume done spine=%d success=%u pagesCreated=%u hasMore=%u suspended=%u aborted=%u",
            spineIndex_, static_cast<unsigned>(success), static_cast<unsigned>(pagesCreated_),
            static_cast<unsigned>(hasMore_), static_cast<unsigned>(parserSuspended),
            static_cast<unsigned>(parserAborted));
    if (noProgressRetryable) {
      LOG_INF(TAG, "[CONTENT][EPUB] parsePages resume retryable no-progress spine=%d", spineIndex_);
    }

    if (!parserSuspended) {
      anchorMap_ = liveParser_->getAnchorMap();
      liveParser_.reset();
      releasePreparedPaths();
      initialized_ = false;
      renderer_.clearWidthCache();
    }

    return success || pagesCreated_ > 0;
  }

  // INIT PATH: first call — extract HTML, normalize, create parser
  // Set up hyphenation language from EPUB metadata
  Hyphenation::setLanguage(epub_->getLanguage());

  const auto localPath = epub_->getSpineItem(spineIndex_).href;

  // Derive chapter base path for resolving relative image paths
  {
    size_t lastSlash = localPath.rfind('/');
    if (lastSlash != std::string::npos) {
      chapterBasePath_ = localPath.substr(0, lastSlash + 1);
    } else {
      chapterBasePath_.clear();
    }
  }

  if (!prepareChapterHtml(shouldAbort)) {
    LOG_ERR(TAG, "[CONTENT][EPUB] parsePages prepare failed spine=%d", spineIndex_);
    return false;
  }

  // Create read callback for extracting images from EPUB
  auto readItemFn = [this, shouldAbort](const std::string& href, Print& out, size_t chunkSize,
                                        const std::function<bool()>& localAbort)
      -> ChapterHtmlSlimParser::ReadItemStatus {
    auto combinedAbort = [&]() -> bool {
      if (localAbort && localAbort()) {
        return true;
      }
      return shouldAbort && shouldAbort();
    };
    switch (epub_->readItemContentsToStreamDetailed(href, out, chunkSize, nullptr, combinedAbort)) {
      case Epub::ItemReadResult::Success:
        return ChapterHtmlSlimParser::ReadItemStatus::Success;
      case Epub::ItemReadResult::Aborted:
        return ChapterHtmlSlimParser::ReadItemStatus::Aborted;
      case Epub::ItemReadResult::NotFound:
        return ChapterHtmlSlimParser::ReadItemStatus::NotFound;
      case Epub::ItemReadResult::WriteError:
        return ChapterHtmlSlimParser::ReadItemStatus::WriteError;
      case Epub::ItemReadResult::ArchiveError:
        return ChapterHtmlSlimParser::ReadItemStatus::ArchiveError;
      case Epub::ItemReadResult::IoError:
      case Epub::ItemReadResult::OpenFailed:
        return ChapterHtmlSlimParser::ReadItemStatus::IoError;
    }
    return ChapterHtmlSlimParser::ReadItemStatus::ArchiveError;
  };

  // Set up callback state for this batch
  onPageComplete_ = onPageComplete;
  maxPages_ = maxPages;
  pagesCreated_ = 0;
  hitMaxPages_ = false;

  // Create the parser with a callback that references our member state
  auto wrappedCallback = [this](std::unique_ptr<Page> page) -> bool {
    if (hitMaxPages_) return false;

    onPageComplete_(std::move(page));
    pagesCreated_++;

    if (maxPages_ > 0 && pagesCreated_ >= maxPages_) {
      hitMaxPages_ = true;
      return false;
    }
    return true;
  };

  auto progressLogger = [this](int progress) {
    LOG_DBG(TAG, "[CONTENT][EPUB] parse progress spine=%d progress=%d", spineIndex_, progress);
  };

  liveParser_.reset(new ChapterHtmlSlimParser(parseHtmlPath_, renderer_, config_, wrappedCallback, progressLogger,
                                              chapterBasePath_, imageCachePath_, readItemFn, quickImageDecode_,
                                              epub_->getCssParser(),
                                              shouldAbort));

  snapix::crashdebug::mark(snapix::crashdebug::CrashPhase::EpubTocParse, static_cast<int16_t>(spineIndex_));
  bool success = liveParser_->parseAndBuildPages();
  initialized_ = true;

  const bool parserSuspended = liveParser_->isSuspended();
  const bool parserAborted = liveParser_->wasAborted();
  const bool noProgressRetryable = pagesCreated_ == 0 && (parserSuspended || parserAborted);
  if (noProgressRetryable) {
    success = false;
  }
  hasMore_ = parserSuspended || parserAborted || (!success && pagesCreated_ > 0);
  LOG_INF(TAG, "[CONTENT][EPUB] parsePages init done spine=%d success=%u pagesCreated=%u hasMore=%u suspended=%u aborted=%u",
          spineIndex_, static_cast<unsigned>(success), static_cast<unsigned>(pagesCreated_),
          static_cast<unsigned>(hasMore_), static_cast<unsigned>(parserSuspended),
          static_cast<unsigned>(parserAborted));
  if (noProgressRetryable) {
    LOG_INF(TAG, "[CONTENT][EPUB] parsePages init retryable no-progress spine=%d", spineIndex_);
  }

  const bool hardInitFailure = !success && pagesCreated_ == 0 && !parserSuspended && !parserAborted;

  // If parser finished (not suspended), clean up
  if (!parserSuspended) {
    anchorMap_ = liveParser_->getAnchorMap();
    liveParser_.reset();
    if (hardInitFailure) {
      invalidatePreparedHtmlCaches("parse-init-failed");
    }
    releasePreparedPaths();
    initialized_ = false;
    renderer_.clearWidthCache();
  }

  return success || pagesCreated_ > 0;
}
