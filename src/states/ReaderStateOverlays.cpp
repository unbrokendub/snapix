#include "ReaderState.h"

#include <Arduino.h>
#include <EpubChapterParser.h>
#include <Fb2.h>
#include <Fb2Parser.h>
#include <GfxRenderer.h>
#include <HtmlParser.h>
#include <Logging.h>
#include <MarkdownParser.h>
#include <Page.h>
#include <PageCache.h>
#include <PlainTextParser.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>

#include "../FontManager.h"
#include "../config.h"
#include "../content/BookmarkManager.h"
#include "../content/ProgressManager.h"
#include "../content/ReaderNavigation.h"
#include "../core/BootMode.h"
#include "../core/CrashDebug.h"
#include "../core/Core.h"
#include "../ui/Elements.h"
#include "../ui/views/ReaderViews.h"
#include "ThemeManager.h"
#include "reader/ReaderStateInternal.h"
#include "reader/ReaderSupport.h"

#define TAG "READER"

namespace papyrix {
using reader::contentCachePath;
using reader::epubSectionCachePath;
using reader::kEpubInteractiveHotExtendBatchPages;
using reader::kPendingEpubPageLoadMaxRetries;
using reader::kPendingTocJumpMaxRetries;

// ============================================================================
// TOC Overlay Mode
// ============================================================================

void ReaderState::enterTocMode(Core& core) {
  if (core.content.tocCount() == 0) {
    return;
  }

  // Stop background task before TOC overlay — both SD card I/O (thumbnail)
  // and e-ink display update share the same SPI bus
  stopBackgroundCaching();

  populateTocView(core);
  int currentIdx = findCurrentTocEntry(core);
  if (currentIdx >= 0) {
    tocView_.setCurrentChapter(static_cast<uint16_t>(currentIdx));
  }

  tocView_.buttons = ui::ButtonBar("Back", "Go", "<<", ">>");
  tocMode_ = true;
  tocOverlayRendered_ = false;
  lastTocScrollOffset_ = -1;
  needsRender_ = true;
  LOG_DBG(TAG, "Entered TOC mode");
}

void ReaderState::exitTocMode() {
  tocMode_ = false;
  tocOverlayRendered_ = false;
  lastTocScrollOffset_ = -1;
  needsRender_ = true;
  LOG_DBG(TAG, "Exited TOC mode");
}

void ReaderState::handleTocInput(Core& core, const Event& e) {
  if (e.button == Button::Power && e.type == EventType::ButtonRelease) {
    if (core.settings.shortPwrBtn == Settings::PowerPageTurn && powerPressStartedMs_ != 0) {
      const uint32_t heldMs = millis() - powerPressStartedMs_;
      if (heldMs < core.settings.getPowerButtonDuration()) {
        tocView_.moveDown();
        needsRender_ = true;
      }
    }
    powerPressStartedMs_ = 0;
    return;
  }

  if (e.type != EventType::ButtonPress && e.type != EventType::ButtonRepeat) return;

  switch (e.button) {
    case Button::Up:
      tocView_.moveUp();
      needsRender_ = true;
      break;

    case Button::Down:
      tocView_.moveDown();
      needsRender_ = true;
      break;

    case Button::Left:
      tocView_.movePageUp(tocVisibleCount());
      needsRender_ = true;
      break;

    case Button::Right:
      tocView_.movePageDown(tocVisibleCount());
      needsRender_ = true;
      break;

    case Button::Center:
      jumpToTocEntry(core, tocView_.selected);
      exitTocMode();
      resumeBackgroundCachingAfterRender_ = true;
      break;

    case Button::Back:
      exitTocMode();
      resumeBackgroundCachingAfterRender_ = true;
      break;

    case Button::Power:
      if (e.type == EventType::ButtonPress && core.settings.shortPwrBtn == Settings::PowerPageTurn) {
        powerPressStartedMs_ = millis();
      }
      break;
  }
}

void ReaderState::populateTocView(Core& core) {
  tocView_.clear();
  const uint16_t count = core.content.tocCount();
  const ContentType type = core.content.metadata().type;

  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) {
      return;
    }

    auto epub = provider->getEpubShared();
    for (uint16_t i = 0; i < count && i < ui::ChapterListView::MAX_CHAPTERS; i++) {
      const auto tocItem = epub->getTocItem(i);
      const int resolvedSpine = epub->resolveTocSpineIndex(i);
      const uint16_t pageNum = (resolvedSpine >= 0) ? static_cast<uint16_t>(resolvedSpine) : 0;
      tocView_.addChapter(tocItem.title.c_str(), pageNum, tocItem.level);
    }
    return;
  }

  for (uint16_t i = 0; i < count && i < ui::ChapterListView::MAX_CHAPTERS; i++) {
    auto result = core.content.getTocEntry(i);
    if (result.ok()) {
      const TocEntry& entry = result.value;
      tocView_.addChapter(entry.title, static_cast<uint16_t>(entry.pageIndex), entry.depth);
    }
  }
}

int ReaderState::findCurrentTocEntry(Core& core) {
  ContentType type = core.content.metadata().type;

  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return -1;
    auto epub = provider->getEpubShared();

    // Start with spine-level match as fallback
    int bestMatch = epub->getTocIndexForSpineIndex(currentSpineIndex_);
    int bestMatchPage = -1;

    // Load anchor map once from disk (avoids reopening file per TOC entry)
    std::string cachePath = epubSectionCachePath(epub->getCachePath(), currentSpineIndex_);
    const auto& anchors = getCachedAnchorMap(cachePath, currentSpineIndex_);

    // Refine: find the latest TOC entry whose anchor page <= current page
    const int tocCount = epub->getTocItemsCount();

    for (int i = 0; i < tocCount; i++) {
      auto tocItem = epub->getTocItem(i);
      const int tocSpineIndex = epub->resolveTocSpineIndex(i);
      if (tocSpineIndex != currentSpineIndex_) continue;

      int entryPage = 0;  // No anchor = start of spine
      if (!tocItem.anchor.empty()) {
        int anchorPage = -1;
        for (const auto& a : anchors) {
          if (a.first == tocItem.anchor) {
            anchorPage = a.second;
            break;
          }
        }
        if (anchorPage < 0) continue;  // Anchor not resolved yet
        entryPage = anchorPage;
      }

      if (entryPage <= currentSectionPage_ && entryPage >= bestMatchPage) {
        bestMatch = i;
        bestMatchPage = entryPage;
      }
    }

    return bestMatch;
  } else if (type == ContentType::Xtc) {
    // For XTC, find chapter containing current page
    const uint16_t count = core.content.tocCount();
    int lastMatch = -1;
    for (uint16_t i = 0; i < count; i++) {
      auto result = core.content.getTocEntry(i);
      if (result.ok() && result.value.pageIndex <= static_cast<uint32_t>(currentPage_)) {
        lastMatch = i;
      }
    }
    return lastMatch;
  } else if (type == ContentType::Fb2) {
    auto* provider = core.content.asFb2();
    if (fb2UsesSectionNavigation(provider)) {
      const int tocCount = static_cast<int>(provider->tocCount());
      if (currentSpineIndex_ >= 0 && currentSpineIndex_ < tocCount) {
        return currentSpineIndex_;
      }
      return tocCount > 0 ? 0 : -1;
    }

    // Legacy fallback for TOC-less FB2 files that still use a flat page cache.
    const Theme& theme = THEME_MANAGER.current();
    const auto vp = getReaderViewport(core.settings.statusBar != 0);
    const auto config = core.settings.getRenderConfig(theme, vp.width, vp.height);
    std::string cachePath = contentCachePath(core.content.cacheDir(), config.fontId);

    const auto& anchors = getCachedAnchorMap(cachePath, 0);

    const uint16_t count = core.content.tocCount();
    int lastMatch = -1;
    for (uint16_t i = 0; i < count; i++) {
      auto result = core.content.getTocEntry(i);
      if (!result.ok()) continue;

      const Fb2::TocItem item = provider && provider->getFb2() ? provider->getFb2()->getTocItem(i) : Fb2::TocItem{};
      std::string anchor = "section_" + std::to_string(item.sectionIndex);
      int entryPage = -1;
      for (const auto& a : anchors) {
        if (a.first == anchor) {
          entryPage = a.second;
          break;
        }
      }
      if (entryPage < 0) continue;
      if (entryPage <= currentSectionPage_) {
        lastMatch = i;
      }
    }
    return lastMatch;
  } else if (type == ContentType::Markdown || type == ContentType::Txt || type == ContentType::Html) {
    // For flat-page formats, find chapter whose pageIndex <= current section page
    const uint16_t count = core.content.tocCount();
    int lastMatch = -1;
    for (uint16_t i = 0; i < count; i++) {
      auto result = core.content.getTocEntry(i);
      if (result.ok() && result.value.pageIndex <= static_cast<uint32_t>(currentSectionPage_)) {
        lastMatch = i;
      }
    }
    return lastMatch;
  }

  return -1;
}

void ReaderState::jumpToTocEntry(Core& core, int tocIndex) {
  if (tocIndex < 0 || tocIndex >= tocView_.chapterCount) {
    return;
  }

  const auto& chapter = tocView_.chapters[tocIndex];
  ContentType type = core.content.metadata().type;

  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return;
    auto epub = provider->getEpubShared();
    auto tocItem = epub->getTocItem(tocIndex);
    const int targetSpine = epub->resolveTocSpineIndex(tocIndex);

    if (targetSpine < 0 || targetSpine >= epub->getSpineItemsCount()) {
      LOG_ERR(TAG, "Unable to resolve TOC target for EPUB entry %d", tocIndex);
      return;
    }

    currentSpineIndex_ = targetSpine;
    parser_.reset();
    parserSpineIndex_ = -1;
    if (lookaheadParserSpineIndex_ != targetSpine) {
      clearLookaheadParser();
    }
    pageCache_.reset();
    invalidateAnchorMapCache();
    clearPagePrefetch();
    currentSectionPage_ = 0;

    std::string cachePath = epubSectionCachePath(epub->getCachePath(), targetSpine);
    int page = -1;
    if (!tocItem.anchor.empty()) {
      const auto& anchors = getCachedAnchorMap(cachePath, targetSpine);
      for (const auto& a : anchors) {
        if (a.first == tocItem.anchor) {
          page = a.second;
          break;
        }
      }
    }

    if (page >= 0) {
      currentSectionPage_ = page;
      pendingTocJumpActive_ = false;
      pendingTocJumpIndexingShown_ = false;
      pendingTocJumpTargetSpine_ = -1;
      pendingTocJumpTargetPageHint_ = -1;
      pendingTocJumpAnchor_.clear();
      pendingTocJumpRetryCount_ = 0;
      clearPendingEpubPageLoad();
      pendingBackgroundEpubRefresh_ = false;
      pendingBackgroundEpubRefreshSpine_ = -1;
      pendingBackgroundEpubRefreshPage_ = -1;
      queuedPendingEpubTurn_ = 0;
      queuedPendingEpubTurnQueuedMs_ = 0;
      lastCachePreemptRequestedMs_ = 0;
    } else {
      crashdebug::mark(crashdebug::CrashPhase::EpubTocSelected, static_cast<int16_t>(targetSpine), 0);
      pendingTocJumpActive_ = true;
      pendingTocJumpIndexingShown_ = false;
      pendingTocJumpTargetSpine_ = targetSpine;
      pendingTocJumpTargetPageHint_ = -1;
      pendingTocJumpAnchor_ = tocItem.anchor;
      pendingTocJumpRetryCount_ = 0;
      pendingTocJumpStartedMs_ = millis();
      pendingTocJumpLastDiagMs_ = 0;
      LOG_INF(TAG, "[ASYNC] arm TOC jump toc=%d spine=%d anchor=%s", tocIndex, targetSpine, tocItem.anchor.c_str());
      clearPendingEpubPageLoad();
    }
  } else if (type == ContentType::Xtc) {
    // For XTC, pageNum is page index
    currentPage_ = chapter.pageNum;
  } else if (type == ContentType::Fb2) {
    auto* provider = core.content.asFb2();
    if (fb2UsesSectionNavigation(provider)) {
      if (currentSpineIndex_ != tocIndex) {
        currentSpineIndex_ = tocIndex;
        parser_.reset();
        parserSpineIndex_ = -1;
        clearLookaheadParser();
        pageCache_.reset();
        invalidateAnchorMapCache();
        clearPagePrefetch();
        resetBackgroundPrefetchState();
      }
      currentSectionPage_ = 0;
      currentPage_ = 0;
      asyncJobs_.clearPendingTocJump();
      clearPendingEpubPageLoad();
      pendingBackgroundEpubRefresh_ = false;
      pendingBackgroundEpubRefreshSpine_ = -1;
      pendingBackgroundEpubRefreshPage_ = -1;
      queuedPendingEpubTurn_ = 0;
      queuedPendingEpubTurnQueuedMs_ = 0;
      lastCachePreemptRequestedMs_ = 0;
    } else {
      // Legacy fallback for TOC-less FB2 files that still use a flat page cache.
      auto* fb2 = provider ? provider->getFb2() : nullptr;
      const Fb2::TocItem tocItem = fb2 ? fb2->getTocItem(static_cast<uint16_t>(tocIndex)) : Fb2::TocItem{};
      const int targetSectionIndex = tocItem.sectionIndex >= 0 ? tocItem.sectionIndex : chapter.pageNum;
      const int targetPageHint = estimateFb2TocTargetPageHint(fb2, provider ? provider->pageCount() : 0, tocItem);
      const Theme& theme = THEME_MANAGER.current();
      const auto vp = getReaderViewport(core.settings.statusBar != 0);
      const auto config = core.settings.getRenderConfig(theme, vp.width, vp.height);
      std::string cachePath = contentCachePath(core.content.cacheDir(), config.fontId);
      std::string anchor = "section_" + std::to_string(targetSectionIndex);

      auto resolveAnchorPage = [&]() -> int {
        invalidateAnchorMapCache();
        for (const auto& a : getCachedAnchorMap(cachePath, 0)) {
          if (a.first == anchor) {
            return a.second;
          }
        }
        return -1;
      };

      int page = resolveAnchorPage();
      if (page >= 0) {
        currentSectionPage_ = page;
        currentPage_ = page;
        asyncJobs_.clearPendingTocJump();
        clearPendingEpubPageLoad();
      } else {
        pendingTocJumpActive_ = true;
        pendingTocJumpIndexingShown_ = false;
        pendingTocJumpTargetSpine_ = 0;
        pendingTocJumpTargetPageHint_ = targetPageHint;
        pendingTocJumpAnchor_ = anchor;
        pendingTocJumpRetryCount_ = 0;
        pendingTocJumpStartedMs_ = millis();
        pendingTocJumpLastDiagMs_ = 0;
        clearPendingEpubPageLoad();
        LOG_INF(TAG, "[ASYNC] arm FB2 TOC jump toc=%d anchor=%s hintPage=%d", tocIndex, anchor.c_str(),
                targetPageHint);
      }
    }
  } else if (type == ContentType::Markdown || type == ContentType::Txt || type == ContentType::Html) {
    // For flat-page formats, pageNum is the section page index
    currentSectionPage_ = chapter.pageNum;
  }

  needsRender_ = true;
  LOG_DBG(TAG, "Jumped to TOC entry %d (spine/page %d)", tocIndex, type == ContentType::Epub ? currentSpineIndex_
                                                                                              : chapter.pageNum);
}

void ReaderState::startPendingTocJumpBackgroundWork(Core& core) {
  if (!pendingTocJumpActive_ || isWorkerRunning()) {
    LOG_INF(TAG, "[ASYNC] TOC worker not started active=%u running=%u state=%d",
            static_cast<unsigned>(pendingTocJumpActive_), static_cast<unsigned>(isWorkerRunning()),
            static_cast<int>(workerState()));
    return;
  }

  const ContentType type = core.content.metadata().type;
  if (type != ContentType::Epub && type != ContentType::Fb2) {
    return;
  }
  if (type == ContentType::Fb2 && fb2UsesSectionNavigation(core.content.asFb2())) {
    asyncJobs_.clearPendingTocJump();
    return;
  }

  const int targetSpine = pendingTocJumpTargetSpine_;
  const std::string targetAnchor = pendingTocJumpAnchor_;
  const int targetPageHint = pendingTocJumpTargetPageHint_;

  pendingTocJumpRetryCount_++;
  if (type == ContentType::Epub) {
    crashdebug::mark(crashdebug::CrashPhase::EpubTocWorkerStart, static_cast<int16_t>(targetSpine),
                     pendingTocJumpRetryCount_);
  }
  LOG_INF(TAG, "[ASYNC] Start TOC worker spine=%d anchor=%s hint=%d attempt=%u", targetSpine, targetAnchor.c_str(),
          targetPageHint, static_cast<unsigned>(pendingTocJumpRetryCount_));

  reader::ReaderAsyncJobsController::TocJumpRequest request;
  request.targetSpine = targetSpine;
  request.targetPageHint = targetPageHint;
  request.retryCount = pendingTocJumpRetryCount_;
  strlcpy(request.anchor, targetAnchor.c_str(), sizeof(request.anchor));
  if (!asyncJobs_.queueTocJumpWork(request)) {
    if (pendingTocJumpRetryCount_ > 0) {
      pendingTocJumpRetryCount_--;
    }
    LOG_INF(TAG, "[ASYNC] TOC worker start failed spine=%d state=%d", targetSpine, static_cast<int>(workerState()));
  }
}

void ReaderState::processPendingTocJump(Core& core) {
  if (!pendingTocJumpActive_) {
    return;
  }
  if (core.content.metadata().type == ContentType::Fb2 && fb2UsesSectionNavigation(core.content.asFb2())) {
    asyncJobs_.clearPendingTocJump();
    return;
  }

  const uint32_t nowMs = millis();
  if (pendingTocJumpLastDiagMs_ == 0 || nowMs - pendingTocJumpLastDiagMs_ >= 1000) {
    LOG_INF(TAG,
            "[ASYNC] TOC pending elapsed=%lu spine=%d anchor=%s hint=%d taskRunning=%u taskState=%d cache=%u pages=%u partial=%u retries=%u",
            static_cast<unsigned long>(pendingTocJumpStartedMs_ ? (nowMs - pendingTocJumpStartedMs_) : 0),
            pendingTocJumpTargetSpine_, pendingTocJumpAnchor_.c_str(), pendingTocJumpTargetPageHint_,
            static_cast<unsigned>(isWorkerRunning()),
            static_cast<int>(workerState()), static_cast<unsigned>(pageCache_ != nullptr),
            static_cast<unsigned>(pageCache_ ? pageCache_->pageCount() : 0),
            static_cast<unsigned>(pageCache_ ? pageCache_->isPartial() : 0),
            static_cast<unsigned>(pendingTocJumpRetryCount_));
    pendingTocJumpLastDiagMs_ = nowMs;
  }

  const ContentType type = core.content.metadata().type;
  std::string cachePath;
  int anchorSpine = 0;

  auto resolveAnchorPage = [&]() -> int {
    if (pendingTocJumpAnchor_.empty()) {
      if (pageCache_ && pageCache_->pageCount() > 0) {
        return 0;
      }
      return -1;
    }
    invalidateAnchorMapCache();
    const auto& anchors = getCachedAnchorMap(cachePath, anchorSpine);
    for (const auto& a : anchors) {
      if (a.first == pendingTocJumpAnchor_) {
        return a.second;
      }
    }
    return -1;
  };

  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) {
      asyncJobs_.clearPendingTocJump();
      return;
    }

    auto epub = provider->getEpubShared();
    if (pendingTocJumpTargetSpine_ < 0 || pendingTocJumpTargetSpine_ >= epub->getSpineItemsCount()) {
      asyncJobs_.clearPendingTocJump();
      needsRender_ = true;
      return;
    }

    const int previousSpineIndex = currentSpineIndex_;
    currentSpineIndex_ = pendingTocJumpTargetSpine_;
    if (currentSpineIndex_ != previousSpineIndex) {
      clearPagePrefetch();
      resetBackgroundPrefetchState();
    }
    anchorSpine = pendingTocJumpTargetSpine_;
    cachePath = epubSectionCachePath(epub->getCachePath(), pendingTocJumpTargetSpine_);
  } else if (type == ContentType::Fb2) {
    const Theme& theme = THEME_MANAGER.current();
    const auto vp = getReaderViewport(core.settings.statusBar != 0);
    const auto config = core.settings.getRenderConfig(theme, vp.width, vp.height);
    currentSpineIndex_ = 0;
    anchorSpine = 0;
    cachePath = contentCachePath(core.content.cacheDir(), config.fontId);
  } else {
    asyncJobs_.clearPendingTocJump();
    needsRender_ = true;
    return;
  }

  int page = resolveAnchorPage();
  if (page >= 0) {
    if (type == ContentType::Fb2 && (!pageCache_ || page >= static_cast<int>(pageCache_->pageCount()))) {
      reloadCacheFromDisk(core);
      if (!pageCache_ || page >= static_cast<int>(pageCache_->pageCount())) {
        currentSectionPage_ = page;
        currentPage_ = page;
        asyncJobs_.clearPendingTocJump();
        armPendingEpubPageLoad(core, 0, page, false, false);
        resumeBackgroundCachingAfterRender_ = false;
        needsRender_ = true;
        return;
      }
    }
    if (type == ContentType::Epub) {
      crashdebug::mark(crashdebug::CrashPhase::EpubTocResolved, static_cast<int16_t>(pendingTocJumpTargetSpine_),
                       pendingTocJumpRetryCount_);
    }
    currentSectionPage_ = page;
    if (type == ContentType::Fb2) {
      currentPage_ = page;
    }
    asyncJobs_.clearPendingTocJump();
    resumeBackgroundCachingAfterRender_ = true;
    needsRender_ = true;
    return;
  }

  if (isWorkerRunning()) {
    vTaskDelay(1 / portTICK_PERIOD_MS);
    return;
  }

  reloadCacheFromDisk(core);

  page = resolveAnchorPage();
  if (page >= 0) {
    if (type == ContentType::Epub) {
      crashdebug::mark(crashdebug::CrashPhase::EpubTocResolved, static_cast<int16_t>(pendingTocJumpTargetSpine_),
                       pendingTocJumpRetryCount_);
    }
    currentSectionPage_ = page;
    if (type == ContentType::Fb2) {
      currentPage_ = page;
    }
    asyncJobs_.clearPendingTocJump();
    resumeBackgroundCachingAfterRender_ = true;
    needsRender_ = true;
    return;
  }

  if ((!pageCache_ || pageCache_->isPartial()) && pendingTocJumpRetryCount_ < kPendingTocJumpMaxRetries) {
    startPendingTocJumpBackgroundWork(core);
    return;
  }

  const bool exhausted = pageCache_ && !pageCache_->isPartial();
  if (exhausted || pendingTocJumpRetryCount_ >= kPendingTocJumpMaxRetries) {
    LOG_ERR(TAG, "Failed to resolve TOC anchor '%s' in spine %d", pendingTocJumpAnchor_.c_str(),
            pendingTocJumpTargetSpine_);
    asyncJobs_.clearPendingTocJump();
    resumeBackgroundCachingAfterRender_ = true;
    needsRender_ = true;
    return;
  }

  // Yield between index chunks so deep TOC jumps don't monopolize the CPU.
  vTaskDelay(1 / portTICK_PERIOD_MS);
}

void ReaderState::startPendingEpubPageLoadBackgroundWork(Core& core) {
  if (!pendingEpubPageLoadActive_ || isWorkerRunning()) {
    LOG_INF(TAG, "[ASYNC] Page worker not started active=%u running=%u state=%d",
            static_cast<unsigned>(pendingEpubPageLoadActive_), static_cast<unsigned>(isWorkerRunning()),
            static_cast<int>(workerState()));
    return;
  }

  const ContentType type = core.content.metadata().type;
  const bool isEpub = type == ContentType::Epub;
  const bool isFlatPaged = type == ContentType::Fb2 || type == ContentType::Markdown || type == ContentType::Txt ||
                           type == ContentType::Html;
  if (!isEpub && !isFlatPaged) {
    return;
  }

  auto* provider = core.content.asEpub();
  if (isEpub && (!provider || !provider->getEpub())) {
    return;
  }

  const bool fb2Sectioned = type == ContentType::Fb2 && fb2UsesSectionNavigation(core.content.asFb2());
  const int targetSpine = (isEpub || fb2Sectioned) ? pendingEpubPageLoadTargetSpine_ : 0;
  const int targetPage = pendingEpubPageLoadTargetPage_;
  const bool requireComplete = pendingEpubPageLoadRequireComplete_;

  const reader::HeapState heapBefore = reader::readHeapState();
  if (pageCache_) {
    pageCache_->clearResidentPages();
    pageCache_.reset();
  }
  const bool keepActiveParser = parser_ && parserSpineIndex_ == targetSpine && parser_->canResume();
  if (!keepActiveParser) {
    parser_.reset();
    parserSpineIndex_ = -1;
  }
  if (!isEpub || lookaheadParserSpineIndex_ != targetSpine) {
    clearLookaheadParser();
  }
  invalidateAnchorMapCache();
  const reader::HeapState heapAfter = reader::readHeapState();
  LOG_INF(TAG, "[ASYNC] page-fill memory trim free=%u->%u largest=%u->%u",
          static_cast<unsigned>(heapBefore.freeBytes), static_cast<unsigned>(heapAfter.freeBytes),
          static_cast<unsigned>(heapBefore.largestBlock), static_cast<unsigned>(heapAfter.largestBlock));

  pendingEpubPageLoadRetryCount_++;
  LOG_INF(TAG, "[ASYNC] Start page worker spine=%d page=%d complete=%u attempt=%u", targetSpine, targetPage,
          static_cast<unsigned>(requireComplete), static_cast<unsigned>(pendingEpubPageLoadRetryCount_));

  reader::ReaderAsyncJobsController::PageFillRequest request;
  request.targetSpine = targetSpine;
  request.targetPage = targetPage;
  request.requireComplete = requireComplete;
  if (!asyncJobs_.queuePageFillWork(request)) {
    if (pendingEpubPageLoadRetryCount_ > 0) {
      pendingEpubPageLoadRetryCount_--;
    }
    LOG_INF(TAG, "[ASYNC] Page worker start failed spine=%d page=%d state=%d", targetSpine, targetPage,
            static_cast<int>(workerState()));
  }
}

void ReaderState::processPendingEpubPageLoad(Core& core) {
  if (!pendingEpubPageLoadActive_) {
    return;
  }

  const uint32_t nowMs = millis();
  if (!pendingEpubPageLoadMessageShown_ && pendingEpubPageLoadStartedMs_ != 0 &&
      (nowMs - pendingEpubPageLoadStartedMs_) >= reader::kPendingPageLoadOverlayDelayMs) {
    renderCenteredStatusMessage(core, pendingEpubPageLoadUseIndexingMessage_ ? "Indexing..." : "Loading...");
    pendingEpubPageLoadMessageShown_ = true;
  }

  if (pendingEpubPageLoadLastDiagMs_ == 0 || nowMs - pendingEpubPageLoadLastDiagMs_ >= 1000) {
    LOG_INF(TAG,
            "[ASYNC] Page pending elapsed=%lu spine=%d page=%d complete=%u taskRunning=%u taskState=%d cache=%u pages=%u partial=%u retries=%u",
            static_cast<unsigned long>(pendingEpubPageLoadStartedMs_ ? (nowMs - pendingEpubPageLoadStartedMs_) : 0),
            pendingEpubPageLoadTargetSpine_, pendingEpubPageLoadTargetPage_,
            static_cast<unsigned>(pendingEpubPageLoadRequireComplete_), static_cast<unsigned>(isWorkerRunning()),
            static_cast<int>(workerState()), static_cast<unsigned>(pageCache_ != nullptr),
            static_cast<unsigned>(pageCache_ ? pageCache_->pageCount() : 0),
            static_cast<unsigned>(pageCache_ ? pageCache_->isPartial() : 0),
            static_cast<unsigned>(pendingEpubPageLoadRetryCount_));
    pendingEpubPageLoadLastDiagMs_ = nowMs;
  }

  const ContentType type = core.content.metadata().type;
  const bool isEpub = type == ContentType::Epub;
  const bool isFlatPaged = type == ContentType::Fb2 || type == ContentType::Markdown || type == ContentType::Txt ||
                           type == ContentType::Html;
  if (!isEpub && !isFlatPaged) {
    LOG_ERR(TAG, "[ASYNC] page pending aborted: unsupported content type=%d", static_cast<int>(type));
    clearPendingEpubPageLoad();
    needsRender_ = true;
    return;
  }

  auto* provider = core.content.asEpub();
  if (isEpub) {
    if (!provider || !provider->getEpub()) {
      LOG_ERR(TAG, "[ASYNC] page pending aborted: no provider");
      clearPendingEpubPageLoad();
      needsRender_ = true;
      return;
    }

    auto epub = provider->getEpubShared();
    if (pendingEpubPageLoadTargetSpine_ < 0 || pendingEpubPageLoadTargetSpine_ >= epub->getSpineItemsCount()) {
      LOG_ERR(TAG, "[ASYNC] page pending aborted: invalid spine=%d", pendingEpubPageLoadTargetSpine_);
      clearPendingEpubPageLoad();
      needsRender_ = true;
      return;
    }

    const int previousSpineIndex = currentSpineIndex_;
    currentSpineIndex_ = pendingEpubPageLoadTargetSpine_;
    if (currentSpineIndex_ != previousSpineIndex) {
      clearPagePrefetch();
      resetBackgroundPrefetchState();
    }
  } else if (type == ContentType::Fb2 && fb2UsesSectionNavigation(core.content.asFb2())) {
    auto* fb2Provider = core.content.asFb2();
    if (!fb2Provider || pendingEpubPageLoadTargetSpine_ < 0 ||
        pendingEpubPageLoadTargetSpine_ >= static_cast<int>(fb2Provider->tocCount())) {
      LOG_ERR(TAG, "[ASYNC] page pending aborted: invalid FB2 section=%d", pendingEpubPageLoadTargetSpine_);
      clearPendingEpubPageLoad();
      needsRender_ = true;
      return;
    }
    const int previousSpineIndex = currentSpineIndex_;
    currentSpineIndex_ = pendingEpubPageLoadTargetSpine_;
    if (currentSpineIndex_ != previousSpineIndex) {
      clearPagePrefetch();
      resetBackgroundPrefetchState();
    }
  } else {
    currentSpineIndex_ = 0;
  }

  if (isWorkerRunning()) {
    vTaskDelay(1 / portTICK_PERIOD_MS);
    return;
  }

  const Theme& theme = THEME_MANAGER.current();
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  const auto config = core.settings.getRenderConfig(theme, vp.width, vp.height);
  const std::string cachePath =
      isEpub
          ? epubSectionCachePath(provider->getEpub()->getCachePath(), currentSpineIndex_)
          : ([&]() {
              auto* fb2Provider = core.content.asFb2();
              std::string fb2CachePath;
              if (type == ContentType::Fb2 &&
                  resolveFb2SectionContext(fb2Provider, config, currentSpineIndex_, &fb2CachePath, nullptr, nullptr)) {
                return fb2CachePath;
              }
              return contentCachePath(core.content.cacheDir(), config.fontId);
            })();
  const bool hasFreshWorkerCache = pageCache_ && pageCache_->path() == cachePath;
  if (!hasFreshWorkerCache) {
    reloadCacheFromDisk(core);
  }

  if (pageCache_) {
    if (pendingEpubPageLoadRequireComplete_) {
      if (!pageCache_->isPartial() && pageCache_->pageCount() > 0) {
        LOG_INF(TAG, "[ASYNC] page pending resolved complete spine=%d pages=%u", currentSpineIndex_,
                static_cast<unsigned>(pageCache_->pageCount()));
        currentSectionPage_ = static_cast<int>(pageCache_->pageCount()) - 1;
        if (type == ContentType::Fb2) {
          currentPage_ = currentSectionPage_;
        }
        clearPendingEpubPageLoad();
        resumeBackgroundCachingAfterRender_ = true;
        needsRender_ = true;
        return;
      }
    } else if (pendingEpubPageLoadTargetPage_ >= 0 &&
               pendingEpubPageLoadTargetPage_ < static_cast<int>(pageCache_->pageCount())) {
      LOG_INF(TAG, "[ASYNC] page pending resolved target spine=%d page=%d cachedPages=%u", currentSpineIndex_,
              pendingEpubPageLoadTargetPage_, static_cast<unsigned>(pageCache_->pageCount()));
      currentSectionPage_ = pendingEpubPageLoadTargetPage_;
      if (type == ContentType::Fb2) {
        currentPage_ = currentSectionPage_;
      }
      clearPendingEpubPageLoad();
      resumeBackgroundCachingAfterRender_ = true;
      needsRender_ = true;
      return;
    }
  }

  // Retry when: no cache, cache partial, or cache "complete" but empty (0 pages).
  // The last case happens when the parser fails on the first attempt (e.g. a
  // transient SD-card read error): the cache file is written with 0 pages and
  // marked complete, which previously prevented any retry.
  const bool cacheUsable = pageCache_ && pageCache_->pageCount() > 0 && !pageCache_->isPartial();
  if (!cacheUsable && pendingEpubPageLoadRetryCount_ < kPendingEpubPageLoadMaxRetries) {
    startPendingEpubPageLoadBackgroundWork(core);
    return;
  }

  if (pageCache_ && pageCache_->pageCount() > 0) {
    currentSectionPage_ = pendingEpubPageLoadRequireComplete_
                              ? static_cast<int>(pageCache_->pageCount()) - 1
                              : std::min(pendingEpubPageLoadTargetPage_, static_cast<int>(pageCache_->pageCount()) - 1);
    if (type == ContentType::Fb2) {
      currentPage_ = currentSectionPage_;
    }
    LOG_INF(TAG, "Falling back to nearest cached page spine=%d page=%d", currentSpineIndex_, currentSectionPage_);
    clearPendingEpubPageLoad();
    resumeBackgroundCachingAfterRender_ = true;
    needsRender_ = true;
    return;
  }

  LOG_ERR(TAG, "Failed to resolve page load for spine=%d page=%d", pendingEpubPageLoadTargetSpine_,
          pendingEpubPageLoadTargetPage_);
  clearPendingEpubPageLoad();
  needsRender_ = true;
}

int ReaderState::tocVisibleCount() const {
  constexpr int startY = 60;
  constexpr int bottomMargin = 70;
  const Theme& theme = THEME_MANAGER.current();
  const int itemHeight = theme.itemHeight + theme.itemSpacing;
  return (renderer_.getScreenHeight() - startY - bottomMargin) / itemHeight;
}

void ReaderState::renderTocOverlay(Core& core) {
  const Theme& theme = THEME_MANAGER.current();
  constexpr int startY = 60;
  constexpr int bottomMargin = 70;
  const int visibleCount = tocVisibleCount();

  // Adjust scroll to keep selection visible
  tocView_.ensureVisible(visibleCount);

  renderer_.clearScreen(theme.backgroundColor);
  renderer_.drawCenteredText(theme.uiFontId, 15, "Chapters", theme.primaryTextBlack, BOLD);

  // Use reader font only when external font is selected (for VN/Thai/CJK support),
  // otherwise use smaller UI font for better chapter list readability
  const ContentType type = core.content.metadata().type;
  const bool hasExternalFont = core.settings.hasExternalReaderFont(theme);
  const int tocFontId =
      (type == ContentType::Xtc || !hasExternalFont) ? theme.uiFontId : core.settings.getReaderFontId(theme);

  const int itemHeight = theme.itemHeight + theme.itemSpacing;
  const int listHeight = renderer_.getScreenHeight() - startY - bottomMargin;
  const bool partialRefresh = tocOverlayRendered_ && tocView_.scrollOffset == lastTocScrollOffset_;

  if (partialRefresh) {
    renderer_.preparePartialUpdateFrame();
    renderer_.clearArea(0, startY, renderer_.getScreenWidth(), listHeight, theme.backgroundColor);
    ui::buttonBar(renderer_, theme, tocView_.buttons);
  } else {
    renderer_.clearScreen(theme.backgroundColor);
    renderer_.drawCenteredText(theme.uiFontId, 15, "Chapters", theme.primaryTextBlack, BOLD);
    ui::buttonBar(renderer_, theme, tocView_.buttons);
  }

  const int end = std::min(tocView_.scrollOffset + visibleCount, static_cast<int>(tocView_.chapterCount));
  for (int i = tocView_.scrollOffset; i < end; i++) {
    const int y = startY + (i - tocView_.scrollOffset) * itemHeight;
    ui::chapterItem(renderer_, theme, tocFontId, y, tocView_.chapters[i].title, tocView_.chapters[i].depth,
                    i == tocView_.selected, i == tocView_.currentChapter);
  }

  renderer_.displayBuffer();
  tocOverlayRendered_ = true;
  lastTocScrollOffset_ = tocView_.scrollOffset;
  core.display.markDirty();
}

StateTransition ReaderState::exitToUI(Core& core) {
  if (directUiTransition_) {
    LOG_INF(TAG, "Exiting to UI via direct state transition");
    core.pendingUiReturnFromReader = true;
    core.pendingReaderReturnState = sourceState_;
    return StateTransition::to(sourceState_);
  }

  LOG_INF(TAG, "Exiting to UI mode via restart");

  // Stop background caching first - BackgroundTask::stop() waits properly
  stopBackgroundCaching();

  // Save progress at last rendered position
  if (contentLoaded_) {
    ProgressManager::Progress progress;
    progress.spineIndex = (lastRenderedSectionPage_ == -1) ? 0 : lastRenderedSpineIndex_;
    progress.sectionPage = (lastRenderedSectionPage_ == -1) ? 0 : lastRenderedSectionPage_;
    progress.flatPage = currentPage_;
    ProgressManager::save(core, core.content.cacheDir(), core.content.metadata().type, progress);
    saveBookmarks(core);
    // Skip pageCache_.reset() and content.close() — ESP.restart() follows,
    // and if stopBackgroundCaching() timed out the task still uses them.
  }

  // Determine return destination from cached transition or fall back to sourceState_
  ReturnTo returnTo = ReturnTo::HOME;
  const auto& transition = getTransition();
  if (transition.isValid()) {
    returnTo = transition.returnTo;
  } else if (sourceState_ == StateId::FileList) {
    returnTo = ReturnTo::FILE_MANAGER;
  }

  // Show notification and restart
  showTransitionNotification("Returning to library...");
  saveTransition(BootMode::UI, nullptr, returnTo);

  // Brief delay to ensure SD writes complete before restart
  vTaskDelay(50 / portTICK_PERIOD_MS);
  ESP.restart();
  return StateTransition::stay(StateId::Reader);
}

// ============================================================================
// Menu Overlay Mode
// ============================================================================

void ReaderState::enterMenuMode(Core& core) {
  stopBackgroundCaching();
  menuView_.show();
  menuMode_ = true;
  needsRender_ = true;
  LOG_DBG(TAG, "Entered menu mode");
}

void ReaderState::exitMenuMode() {
  menuView_.hide();
  menuMode_ = false;
  needsRender_ = true;
  LOG_DBG(TAG, "Exited menu mode");
}

void ReaderState::handleMenuInput(Core& core, const Event& e) {
  if (e.type != EventType::ButtonPress) return;

  switch (e.button) {
    case Button::Up:
      menuView_.moveUp();
      needsRender_ = true;
      break;
    case Button::Down:
      menuView_.moveDown();
      needsRender_ = true;
      break;
    case Button::Center:
      handleMenuAction(core, menuView_.selected);
      break;
    case Button::Back:
      exitMenuMode();
      resumeBackgroundCachingAfterRender_ = true;
      break;
    default:
      break;
  }
}

void ReaderState::handleMenuAction(Core& core, int action) {
  exitMenuMode();
  switch (action) {
    case 0:  // Chapters
      if (core.content.tocCount() > 0) {
        enterTocMode(core);
      } else {
        const Theme& theme = THEME_MANAGER.current();
        renderer_.clearScreen(theme.backgroundColor);
        ui::overlayBox(renderer_, theme, core.settings.getReaderFontId(theme), renderer_.getScreenHeight() / 2 - 20,
                       "No chapters");
        renderer_.displayBuffer();
        core.display.markDirty();
        resumeBackgroundCachingAfterRender_ = true;
      }
      break;
    case 1:  // Bookmarks
      enterBookmarkMode(core);
      break;
  }
}

// ============================================================================
// Bookmark Overlay Mode
// ============================================================================

void ReaderState::enterBookmarkMode(Core& core) {
  stopBackgroundCaching();
  populateBookmarkView();
  bookmarkView_.buttons = ui::ButtonBar("Back", "Go", "Add", "Del");
  bookmarkMode_ = true;
  bookmarkOverlayRendered_ = false;
  lastBookmarkScrollOffset_ = -1;
  needsRender_ = true;
  LOG_DBG(TAG, "Entered bookmark mode (%d bookmarks)", bookmarkCount_);
}

void ReaderState::exitBookmarkMode() {
  bookmarkMode_ = false;
  bookmarkOverlayRendered_ = false;
  lastBookmarkScrollOffset_ = -1;
  needsRender_ = true;
  LOG_DBG(TAG, "Exited bookmark mode");
}

void ReaderState::handleBookmarkInput(Core& core, const Event& e) {
  if (e.type != EventType::ButtonPress && e.type != EventType::ButtonRepeat) return;

  switch (e.button) {
    case Button::Up:
      bookmarkView_.moveUp();
      needsRender_ = true;
      break;
    case Button::Down:
      bookmarkView_.moveDown();
      needsRender_ = true;
      break;
    case Button::Center:
      if (bookmarkCount_ > 0) {
        jumpToBookmark(core, bookmarkView_.selected);
        exitBookmarkMode();
        resumeBackgroundCachingAfterRender_ = true;
      }
      break;
    case Button::Left:
      addBookmark(core);
      break;
    case Button::Right:
      if (bookmarkCount_ > 0) {
        deleteBookmark(core, bookmarkView_.selected);
      }
      break;
    case Button::Back:
      exitBookmarkMode();
      resumeBackgroundCachingAfterRender_ = true;
      break;
    default:
      break;
  }
}

void ReaderState::renderBookmarkOverlay(Core& core) {
  const Theme& theme = THEME_MANAGER.current();
  constexpr int startY = 60;
  constexpr int bottomMargin = 70;
  const int visibleCount = bookmarkVisibleCount();

  bookmarkView_.ensureVisible(visibleCount);

  const int listHeight = renderer_.getScreenHeight() - startY - bottomMargin;
  const bool partialRefresh = bookmarkOverlayRendered_ && bookmarkView_.scrollOffset == lastBookmarkScrollOffset_ &&
                              bookmarkCount_ > 0;

  if (partialRefresh) {
    renderer_.preparePartialUpdateFrame();
    renderer_.clearArea(0, startY, renderer_.getScreenWidth(), listHeight, theme.backgroundColor);
    ui::buttonBar(renderer_, theme, bookmarkView_.buttons);
  } else {
    renderer_.clearScreen(theme.backgroundColor);
    renderer_.drawCenteredText(theme.uiFontId, 15, "Bookmarks", theme.primaryTextBlack, BOLD);
    ui::buttonBar(renderer_, theme, bookmarkView_.buttons);
  }

  if (bookmarkCount_ == 0) {
    const int y = renderer_.getScreenHeight() / 2 - renderer_.getLineHeight(theme.uiFontId) / 2;
    renderer_.drawCenteredText(theme.uiFontId, y, "No bookmarks yet", theme.primaryTextBlack, BOLD);
  } else {
    const int itemHeight = theme.itemHeight + theme.itemSpacing;
    const int end = std::min(bookmarkView_.scrollOffset + visibleCount, static_cast<int>(bookmarkView_.itemCount));
    for (int i = bookmarkView_.scrollOffset; i < end; i++) {
      const int y = startY + (i - bookmarkView_.scrollOffset) * itemHeight;
      ui::chapterItem(renderer_, theme, theme.uiFontId, y, bookmarkView_.items[i].title, bookmarkView_.items[i].depth,
                      i == bookmarkView_.selected, false);
    }
  }

  renderer_.displayBuffer();
  bookmarkOverlayRendered_ = true;
  lastBookmarkScrollOffset_ = bookmarkView_.scrollOffset;
  core.display.markDirty();
}

void ReaderState::addBookmark(Core& core) {
  if (bookmarkCount_ >= BookmarkManager::MAX_BOOKMARKS) {
    LOG_DBG(TAG, "Max bookmarks reached");
    return;
  }

  ContentType type = core.content.metadata().type;

  int existing = BookmarkManager::findAt(bookmarks_, bookmarkCount_, type, lastRenderedSpineIndex_,
                                         lastRenderedSectionPage_, currentPage_);
  if (existing >= 0) {
    LOG_DBG(TAG, "Bookmark already exists at index %d", existing);
    return;
  }

  Bookmark bm;
  memset(&bm, 0, sizeof(bm));
  bm.spineIndex = static_cast<int16_t>(lastRenderedSpineIndex_);
  bm.sectionPage = static_cast<int16_t>(lastRenderedSectionPage_);
  bm.flatPage = currentPage_;

  if (cachedChapterTitle_[0] != '\0') {
    strncpy(bm.label, cachedChapterTitle_, sizeof(bm.label) - 1);
    bm.label[sizeof(bm.label) - 1] = '\0';
  } else {
    if (type == ContentType::Xtc) {
      snprintf(bm.label, sizeof(bm.label), "Page %u", static_cast<unsigned>(currentPage_ + 1));
    } else {
      snprintf(bm.label, sizeof(bm.label), "Page %d", lastRenderedSectionPage_ + 1);
    }
  }

  int insertPos = bookmarkCount_;
  for (int i = 0; i < bookmarkCount_; i++) {
    bool insertHere = false;
    if (type == ContentType::Epub || type == ContentType::Fb2) {
      if (bm.spineIndex < bookmarks_[i].spineIndex ||
          (bm.spineIndex == bookmarks_[i].spineIndex && bm.sectionPage < bookmarks_[i].sectionPage)) {
        insertHere = true;
      }
    } else if (type == ContentType::Xtc) {
      if (bm.flatPage < bookmarks_[i].flatPage) {
        insertHere = true;
      }
    } else {
      if (bm.sectionPage < bookmarks_[i].sectionPage) {
        insertHere = true;
      }
    }
    if (insertHere) {
      insertPos = i;
      break;
    }
  }

  for (int i = bookmarkCount_; i > insertPos; i--) {
    bookmarks_[i] = bookmarks_[i - 1];
  }
  bookmarks_[insertPos] = bm;
  bookmarkCount_++;

  saveBookmarks(core);
  populateBookmarkView();
  needsRender_ = true;
  LOG_DBG(TAG, "Added bookmark at position %d", insertPos);
}

void ReaderState::deleteBookmark(Core& core, int index) {
  if (index < 0 || index >= bookmarkCount_) return;

  for (int i = index; i < bookmarkCount_ - 1; i++) {
    bookmarks_[i] = bookmarks_[i + 1];
  }
  bookmarkCount_--;

  saveBookmarks(core);
  populateBookmarkView();

  if (bookmarkView_.selected >= bookmarkView_.itemCount && bookmarkView_.itemCount > 0) {
    bookmarkView_.selected = bookmarkView_.itemCount - 1;
  }

  needsRender_ = true;
  LOG_DBG(TAG, "Deleted bookmark at index %d, %d remaining", index, bookmarkCount_);
}

void ReaderState::jumpToBookmark(Core& core, int index) {
  if (index < 0 || index >= bookmarkCount_) return;

  const Bookmark& bm = bookmarks_[index];
  ContentType type = core.content.metadata().type;

  if (type == ContentType::Epub || type == ContentType::Fb2) {
    if (bm.spineIndex != currentSpineIndex_) {
      currentSpineIndex_ = bm.spineIndex;
      parser_.reset();
      parserSpineIndex_ = -1;
      if (lookaheadParserSpineIndex_ != currentSpineIndex_) {
        clearLookaheadParser();
      }
      pageCache_.reset();
      invalidateAnchorMapCache();
      clearPagePrefetch();
      resetBackgroundPrefetchState();
    }
    currentSectionPage_ = bm.sectionPage;
    if (type == ContentType::Fb2) {
      currentPage_ = bm.sectionPage;
    }
  } else if (type == ContentType::Xtc) {
    currentPage_ = bm.flatPage;
  } else {
    currentSectionPage_ = bm.sectionPage;
  }

  needsRender_ = true;
  LOG_DBG(TAG, "Jumped to bookmark %d", index);
}

void ReaderState::saveBookmarks(Core& core) {
  BookmarkManager::save(core, core.content.cacheDir(), core.content.metadata().type, bookmarks_, bookmarkCount_);
}

void ReaderState::populateBookmarkView() {
  bookmarkView_.clear();
  for (int i = 0; i < bookmarkCount_ && i < ui::BookmarkListView::MAX_ITEMS; i++) {
    bookmarkView_.addItem(bookmarks_[i].label, 0);
  }
}

int ReaderState::bookmarkVisibleCount() const {
  constexpr int startY = 60;
  constexpr int bottomMargin = 70;
  const Theme& theme = THEME_MANAGER.current();
  const int itemHeight = theme.itemHeight + theme.itemSpacing;
  return (renderer_.getScreenHeight() - startY - bottomMargin) / itemHeight;
}

}  // namespace papyrix

