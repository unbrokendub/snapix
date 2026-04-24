#include "ReaderState.h"

#include <Arduino.h>
#include <ContentParser.h>
#include <CoverHelpers.h>
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
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>

#include "../Battery.h"
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
using reader::kCacheTaskStopTimeoutMs;
using reader::kIdleBackgroundKickIntervalMs;

namespace {
constexpr int horizontalPadding = 5;
constexpr int statusBarMargin = 23;
constexpr size_t kEstimatedBytesPerPage = 2048;

uint16_t estimatePagesForBytes(const size_t bytes, const size_t bytesPerPage = kEstimatedBytesPerPage) {
  const size_t safeBytesPerPage = std::max<size_t>(1, bytesPerPage);
  const size_t pageCount = std::max<size_t>(1, (bytes + safeBytesPerPage - 1) / safeBytesPerPage);
  return static_cast<uint16_t>(std::min<size_t>(pageCount, UINT16_MAX));
}
}  // namespace

void ReaderState::clearPagePrefetch() {
  cacheController_.clearPagePrefetch();
}

void ReaderState::clearLookaheadParser() {
  cacheController_.clearLookaheadParser();
}

void ReaderState::resetBackgroundPrefetchState() { cacheController_.resetBackgroundPrefetchState(); }

const char* ReaderState::backgroundCacheWakeReasonToString(const BackgroundCacheWakeReason reason) {
  return reader::ReaderCacheController::backgroundCacheWakeReasonToString(reason);
}

bool ReaderState::promoteLookaheadParser(const int targetSpine) {
  const bool promoted = cacheController_.promoteLookaheadParser(targetSpine);
  if (promoted) {
    LOG_DBG(TAG, "Promoted lookahead EPUB parser spine=%d", targetSpine);
  }
  return promoted;
}

void ReaderState::prefetchAdjacentPage(Core& core) { cacheController_.prefetchAdjacentPage(core); }

void ReaderState::clearPendingEpubPageLoad() { asyncJobs_.clearPendingPageLoad(); }

void ReaderState::enqueuePendingPageTurn(const int direction, const char* reason) {
  navigationController_.enqueuePendingPageTurn(direction, reason, static_cast<int>(workerState()));
}

bool ReaderState::deferPageTurnUntilCacheStops(const int direction) {
  return navigationController_.deferPageTurnUntilWorkerStops(
      direction, isWorkerRunning(), static_cast<int>(workerState()), [this]() { requestWorkerCancel(); });
}

void ReaderState::armPendingEpubPageLoad(Core& core, const int targetSpine, const int targetPage,
                                         const bool requireComplete, const bool useIndexingMessage) {
  (void)core;
  const bool sameRequest = pendingEpubPageLoadActive_ && pendingEpubPageLoadTargetSpine_ == targetSpine &&
                           pendingEpubPageLoadTargetPage_ == targetPage &&
                           pendingEpubPageLoadRequireComplete_ == requireComplete &&
                           pendingEpubPageLoadUseIndexingMessage_ == useIndexingMessage;

  asyncJobs_.armPendingPageLoad(targetSpine, targetPage, requireComplete, useIndexingMessage);
  if (!sameRequest) {
    pendingEpubPageLoadStartedMs_ = millis();
    pendingEpubPageLoadLastDiagMs_ = 0;
    LOG_INF(TAG, "[ASYNC] arm page-load spine=%d page=%d complete=%u indexing=%u", targetSpine, targetPage,
            static_cast<unsigned>(requireComplete), static_cast<unsigned>(useIndexingMessage));
    pendingEpubPageLoadMessageShown_ = false;
  }
}

void ReaderState::saveAnchorMap(const ContentParser& parser, const std::string& cachePath) {
  reader::ReaderCacheController::saveAnchorMap(parser, cachePath);
}

int ReaderState::loadAnchorPage(const std::string& cachePath, const std::string& anchor) {
  return reader::ReaderCacheController::loadAnchorPage(cachePath, anchor);
}

std::vector<std::pair<std::string, uint16_t>> ReaderState::loadAnchorMap(const std::string& cachePath) {
  return reader::ReaderCacheController::loadAnchorMap(cachePath);
}

const std::vector<std::pair<std::string, uint16_t>>& ReaderState::getCachedAnchorMap(const std::string& cachePath,
                                                                                      const int spineIndex) {
  return cacheController_.getCachedAnchorMap(cachePath, spineIndex);
}

void ReaderState::invalidateAnchorMapCache() { cacheController_.invalidateAnchorMapCache(); }

void ReaderState::invalidateGlobalPageMetrics() {
  globalSectionPageMetrics_.clear();
  globalSectionPageMetrics_.shrink_to_fit();
  globalSectionPageMetricTotal_ = 0;
  globalSectionPageMetricsInitialized_ = false;
}

void ReaderState::recomputeGlobalPageMetricTotal() {
  uint32_t total = 0;
  for (const auto& metric : globalSectionPageMetrics_) {
    total += metric.pages;
  }
  globalSectionPageMetricTotal_ = total;
}

void ReaderState::initializeGlobalPageMetrics(Core& core, const int currentSectionTotalPages,
                                              const bool currentSectionIsPartial) {
  invalidateGlobalPageMetrics();

  const ContentType type = core.content.metadata().type;

  const Theme& theme = THEME_MANAGER.current();
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  const auto config = core.settings.getRenderConfig(theme, vp.width, vp.height);

  int spineCount = 0;
  std::vector<size_t> itemSizes;

  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return;
    auto epub = provider->getEpubShared();
    spineCount = epub->getSpineItemsCount();
    if (spineCount <= 0) return;

    itemSizes.resize(static_cast<size_t>(spineCount), 0);
    for (int i = 0; i < spineCount; ++i) {
      const auto spineItem = epub->getSpineItem(i);
      size_t itemSize = 0;
      if (epub->getItemSize(spineItem.href, &itemSize)) {
        itemSizes[static_cast<size_t>(i)] = itemSize;
      }
    }
  } else if (type == ContentType::Fb2 && fb2UsesSectionNavigation(core.content.asFb2())) {
    auto* provider = core.content.asFb2();
    auto* fb2 = provider ? provider->getFb2() : nullptr;
    if (!fb2 || fb2->tocCount() == 0) return;

    spineCount = static_cast<int>(fb2->tocCount());
    itemSizes.resize(static_cast<size_t>(spineCount), 0);

    // Derive per-section byte sizes from consecutive sourceOffset deltas.
    // For the last section we cannot use totalFileSize because FB2 files
    // typically have large <binary> blocks (base64 cover images, ~60% of
    // file size) after the closing </body>.  Using totalFileSize as the
    // end boundary makes the last section appear 10-30x larger than it
    // really is.  Instead, cap it using the median of preceding sections.
    for (int i = 0; i < spineCount; ++i) {
      const Fb2::TocItem item = fb2->getTocItem(static_cast<uint16_t>(i));
      if (i + 1 < spineCount) {
        const Fb2::TocItem next = fb2->getTocItem(static_cast<uint16_t>(i + 1));
        itemSizes[static_cast<size_t>(i)] =
            next.sourceOffset > item.sourceOffset ? next.sourceOffset - item.sourceOffset : 0;
      }
      // Last section: leave at 0 for now, estimate below.
    }
    if (spineCount > 1) {
      // Estimate the last section from the median of non-zero deltas.
      std::vector<size_t> nonZeroSizes;
      nonZeroSizes.reserve(static_cast<size_t>(spineCount));
      for (int i = 0; i < spineCount - 1; ++i) {
        if (itemSizes[static_cast<size_t>(i)] > 0) {
          nonZeroSizes.push_back(itemSizes[static_cast<size_t>(i)]);
        }
      }
      if (!nonZeroSizes.empty()) {
        std::sort(nonZeroSizes.begin(), nonZeroSizes.end());
        const size_t median = nonZeroSizes[nonZeroSizes.size() / 2];
        itemSizes[static_cast<size_t>(spineCount - 1)] = median;
      }
    } else if (spineCount == 1) {
      // Single-section book: use the first section offset to content start.
      const Fb2::TocItem item = fb2->getTocItem(0);
      const size_t contentEstimate = fb2->getFileSize() > item.sourceOffset
          ? (fb2->getFileSize() - item.sourceOffset) / 3  // crude: ~1/3 of FB2 is text
          : 0;
      itemSizes[0] = contentEstimate;
    }
  } else {
    return;
  }

  globalSectionPageMetrics_.resize(static_cast<size_t>(spineCount));

  size_t calibrationBytes = 0;
  uint32_t calibrationPages = 0;

  for (int spineIndex = 0; spineIndex < spineCount; ++spineIndex) {
    uint16_t pages = 0;
    bool exact = false;

    const std::string cachePath = (type == ContentType::Epub)
        ? reader::epubSectionCachePath(core.content.asEpub()->getEpub()->getCachePath(), spineIndex)
        : reader::fb2SectionCachePath(core.content.asFb2()->getFb2()->getCachePath(), config.fontId, spineIndex);
    const auto probe = PageCache::probe(cachePath, config, false);
    if (probe.valid && probe.pageCount > 0) {
      pages = probe.pageCount;
      exact = !probe.partial;
    }

    if (spineIndex == currentSpineIndex_ && currentSectionTotalPages > 0) {
      const auto currentPages = static_cast<uint16_t>(std::max(1, currentSectionTotalPages));
      if (currentPages > pages || !exact) {
        pages = std::max(pages, currentPages);
        if (!currentSectionIsPartial) {
          exact = true;
        }
      }
    }

    const size_t itemSize = itemSizes[static_cast<size_t>(spineIndex)];
    if (pages > 0) {
      auto& metric = globalSectionPageMetrics_[static_cast<size_t>(spineIndex)];
      metric.pages = pages;
      metric.exact = exact;
      if (itemSize > 0) {
        calibrationBytes += itemSize;
        calibrationPages += pages;
      }
    }
  }

  const size_t bytesPerPage =
      calibrationPages > 0 ? std::max<size_t>(256, calibrationBytes / calibrationPages) : kEstimatedBytesPerPage;

  for (int spineIndex = 0; spineIndex < spineCount; ++spineIndex) {
    auto& metric = globalSectionPageMetrics_[static_cast<size_t>(spineIndex)];
    const size_t itemSize = itemSizes[static_cast<size_t>(spineIndex)];
    const uint16_t estimatedPages = estimatePagesForBytes(itemSize, bytesPerPage);
    if (metric.pages == 0) {
      metric.pages = estimatedPages;
    } else if (!metric.exact && estimatedPages > metric.pages) {
      metric.pages = estimatedPages;
    }
  }

  recomputeGlobalPageMetricTotal();
  globalSectionPageMetricsInitialized_ = true;
}

void ReaderState::updateGlobalPageMetrics(Core& core, const int currentSectionTotalPages, const bool currentSectionIsPartial) {
  const ContentType type = core.content.metadata().type;
  if (type != ContentType::Epub && !(type == ContentType::Fb2 && fb2UsesSectionNavigation(core.content.asFb2()))) {
    return;
  }

  if (!globalSectionPageMetricsInitialized_) {
    initializeGlobalPageMetrics(core, currentSectionTotalPages, currentSectionIsPartial);
  }

  if (!globalSectionPageMetricsInitialized_ || currentSpineIndex_ < 0 ||
      currentSpineIndex_ >= static_cast<int>(globalSectionPageMetrics_.size()) || currentSectionTotalPages <= 0) {
    return;
  }

  auto& metric = globalSectionPageMetrics_[static_cast<size_t>(currentSpineIndex_)];
  const auto currentPages = static_cast<uint16_t>(std::max(1, currentSectionTotalPages));
  bool changed = false;

  if (!currentSectionIsPartial) {
    if (!metric.exact || metric.pages != currentPages) {
      metric.pages = currentPages;
      metric.exact = true;
      changed = true;
    }
  } else if (!metric.exact && currentPages > metric.pages) {
    metric.pages = currentPages;
    changed = true;
  }

  if (changed) {
    recomputeGlobalPageMetricTotal();
  }
}

ReaderState::GlobalPageMetrics ReaderState::resolveGlobalPageMetrics(Core& core, const int currentSectionTotalPages,
                                                                     const bool currentSectionIsPartial) {
  GlobalPageMetrics metrics;
  const ContentType type = core.content.metadata().type;

  if (type == ContentType::Xtc) {
    metrics.currentPage = static_cast<int>(currentPage_) + 1;
    metrics.totalPages = static_cast<int>(std::max<uint32_t>(core.content.pageCount(), metrics.currentPage));
    return metrics;
  }

  if (type == ContentType::Epub) {
    updateGlobalPageMetrics(core, currentSectionTotalPages, currentSectionIsPartial);
    if (globalSectionPageMetricsInitialized_ && !globalSectionPageMetrics_.empty()) {
      const int clampedSpine = std::clamp(currentSpineIndex_, 0, static_cast<int>(globalSectionPageMetrics_.size()) - 1);
      uint32_t pagesBefore = 0;
      for (int i = 0; i < clampedSpine; ++i) {
        pagesBefore += globalSectionPageMetrics_[static_cast<size_t>(i)].pages;
      }
      metrics.currentPage = static_cast<int>(pagesBefore) + std::max(currentSectionPage_, 0) + 1;
      metrics.totalPages = static_cast<int>(std::max<uint32_t>(globalSectionPageMetricTotal_, metrics.currentPage));
      return metrics;
    }
  }

  if (type == ContentType::Fb2 && fb2UsesSectionNavigation(core.content.asFb2())) {
    updateGlobalPageMetrics(core, currentSectionTotalPages, currentSectionIsPartial);
    if (globalSectionPageMetricsInitialized_ && !globalSectionPageMetrics_.empty()) {
      const int clampedSpine = std::clamp(currentSpineIndex_, 0, static_cast<int>(globalSectionPageMetrics_.size()) - 1);
      uint32_t pagesBefore = 0;
      for (int i = 0; i < clampedSpine; ++i) {
        pagesBefore += globalSectionPageMetrics_[static_cast<size_t>(i)].pages;
      }
      metrics.currentPage = static_cast<int>(pagesBefore) + std::max(currentSectionPage_, 0) + 1;
      metrics.totalPages = static_cast<int>(std::max<uint32_t>(globalSectionPageMetricTotal_, metrics.currentPage));
      return metrics;
    }

    // Fallback: per-section metrics not available yet — use crude scaling.
    auto* provider = core.content.asFb2();
    auto* fb2 = provider ? provider->getFb2() : nullptr;
    const int estimatedTotalPages =
        std::max({static_cast<int>(core.content.pageCount()), std::max(currentSectionTotalPages, 0), 1});

    int currentPage = std::max(currentSectionPage_, 0) + 1;
    if (fb2 && currentSpineIndex_ >= 0 && currentSpineIndex_ < static_cast<int>(fb2->tocCount()) && fb2->getFileSize() > 0) {
      const Fb2::TocItem item = fb2->getTocItem(static_cast<uint16_t>(currentSpineIndex_));
      const uint64_t scaled = static_cast<uint64_t>(estimatedTotalPages) * item.sourceOffset;
      currentPage = static_cast<int>(scaled / fb2->getFileSize()) + std::max(currentSectionPage_, 0) + 1;
    }

    metrics.currentPage = std::max(1, currentPage);
    metrics.totalPages = std::max(estimatedTotalPages, metrics.currentPage);
    return metrics;
  }

  metrics.currentPage = std::max(currentSectionPage_, 0) + 1;
  metrics.totalPages = static_cast<int>(std::max<uint32_t>(core.content.pageCount(), metrics.currentPage));
  return metrics;
}

int ReaderState::calcFirstContentSpine(bool hasCover, int textStartIndex, size_t spineCount) {
  return reader::ReaderCacheController::calcFirstContentSpine(hasCover, textStartIndex, spineCount);
}

void ReaderState::createOrExtendCacheImpl(ContentParser& parser, const std::string& cachePath,
                                          const RenderConfig& config, uint16_t batchSize) {
  cacheController_.createOrExtendCacheImpl(parser, cachePath, config, batchSize);
}

ReaderState::BackgroundCachePlan ReaderState::planBackgroundCacheWork(Core& core) { return cacheController_.planBackgroundCacheWork(core); }

bool ReaderState::shouldContinueIdleBackgroundCaching(Core& core) {
  return cacheController_.shouldContinueIdleBackgroundCaching(core);
}

bool ReaderState::prefetchNextEpubSpineCache(Core& core, const RenderConfig& config, const int activeSpineIndex,
                                             const bool coverExists, const int textStartIndex,
                                             const bool allowFarSweep, const std::function<bool()>& shouldAbort) {
  return cacheController_.prefetchNextEpubSpineCache(core, config, activeSpineIndex, coverExists, textStartIndex,
                                                     allowFarSweep, shouldAbort);
}

ReaderState::ReaderState(GfxRenderer& renderer)
    : renderer_(renderer),
      xtcRenderer_(renderer),
      currentPage_(0),
      needsRender_(true),
      contentLoaded_(false),
      currentSpineIndex_(0),
      currentSectionPage_(0),
      cacheController_(renderer_,
                       reader::PositionRefs{currentPage_, currentSpineIndex_, currentSectionPage_, lastRenderedSpineIndex_,
                                            lastRenderedSectionPage_, hasCover_, textStartIndex_}),
      pageCache_(cacheController_.resourceState().pageCache),
      parser_(cacheController_.resourceState().parser),
      parserSpineIndex_(cacheController_.resourceState().parserSpineIndex),
      lookaheadParser_(cacheController_.resourceState().lookaheadParser),
      lookaheadParserSpineIndex_(cacheController_.resourceState().lookaheadParserSpineIndex),
      pagesUntilFullRefresh_(1),
      thumbnailDone_(cacheController_.thumbnailDoneRef()),
      lastIdleBackgroundKickMs_(cacheController_.lastIdleBackgroundKickMsRef()),
      lastReaderInteractionMs_(cacheController_.lastReaderInteractionMsRef()),
      holdNavigated_(navigationController_.holdNavigatedRef()),
      powerPressStartedMs_(navigationController_.powerPressStartedMsRef()),
      warmedNextPage_(cacheController_.warmedNextPageRef()),
      warmedNextNextPage_(cacheController_.warmedNextNextPageRef()),
      renderOverridePage_(cacheController_.renderOverridePageRef()),
      pendingTocJumpActive_(asyncJobs_.pendingTocJumpActiveRef()),
      pendingTocJumpIndexingShown_(asyncJobs_.pendingTocJumpIndexingShownRef()),
      pendingTocJumpTargetSpine_(asyncJobs_.pendingTocJumpTargetSpineRef()),
      pendingTocJumpTargetPageHint_(asyncJobs_.pendingTocJumpTargetPageHintRef()),
      pendingTocJumpAnchor_(asyncJobs_.pendingTocJumpAnchorRef()),
      pendingTocJumpRetryCount_(asyncJobs_.pendingTocJumpRetryCountRef()),
      pendingTocJumpStartedMs_(asyncJobs_.pendingTocJumpStartedMsRef()),
      pendingTocJumpLastDiagMs_(asyncJobs_.pendingTocJumpLastDiagMsRef()),
      pendingEpubPageLoadActive_(asyncJobs_.pendingPageLoadActiveRef()),
      pendingEpubPageLoadMessageShown_(asyncJobs_.pendingPageLoadMessageShownRef()),
      pendingEpubPageLoadRequireComplete_(asyncJobs_.pendingPageLoadRequireCompleteRef()),
      pendingEpubPageLoadUseIndexingMessage_(asyncJobs_.pendingPageLoadUseIndexingMessageRef()),
      pendingEpubPageLoadTargetSpine_(asyncJobs_.pendingPageLoadTargetSpineRef()),
      pendingEpubPageLoadTargetPage_(asyncJobs_.pendingPageLoadTargetPageRef()),
      pendingEpubPageLoadRetryCount_(asyncJobs_.pendingPageLoadRetryCountRef()),
      pendingEpubPageLoadStartedMs_(asyncJobs_.pendingPageLoadStartedMsRef()),
      pendingEpubPageLoadLastDiagMs_(asyncJobs_.pendingPageLoadLastDiagMsRef()),
      pendingBackgroundEpubRefresh_(asyncJobs_.pendingRefresh().active),
      pendingBackgroundEpubRefreshSpine_(asyncJobs_.pendingRefresh().spine),
      pendingBackgroundEpubRefreshPage_(asyncJobs_.pendingRefresh().page),
      queuedPendingEpubTurn_(navigationController_.queuedPendingPageTurnRef()),
      queuedPendingEpubTurnQueuedMs_(navigationController_.queuedPendingPageTurnQueuedMsRef()),
      lastCachePreemptRequestedMs_(navigationController_.lastCachePreemptRequestedMsRef()),
      tocView_{} {
  asyncJobs_.setBackgroundCacheHandler(
      [this](const reader::ReaderAsyncJobsController::BackgroundCacheRequest& request,
             const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort) {
        runBackgroundCacheJob(request, shouldAbort);
      });
  asyncJobs_.setTocJumpHandler([this](const reader::ReaderAsyncJobsController::TocJumpRequest& request,
                                      const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort) {
    runTocJumpJob(request, shouldAbort);
  });
  asyncJobs_.setPageFillHandler([this](const reader::ReaderAsyncJobsController::PageFillRequest& request,
                                       const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort) {
    runPageFillJob(request, shouldAbort);
  });
  contentPath_[0] = '\0';
}

ReaderState::~ReaderState() {
  stopBackgroundCaching();
  asyncJobs_.stopWorker();
}

void ReaderState::setContentPath(const char* path) {
  if (path) {
    strncpy(contentPath_, path, sizeof(contentPath_) - 1);
    contentPath_[sizeof(contentPath_) - 1] = '\0';
  } else {
    contentPath_[0] = '\0';
  }
  cacheController_.setContentPath(contentPath_);
}

void ReaderState::enter(Core& core) {
  // Free memory from the previous reader session before loading a new book.
  // On ESP32 the heap is non-compacting, so unloading long-lived font caches is
  // one of the few effective ways to recover a larger contiguous block.
  //
  // Skip the font unload (and width-cache invalidation) when the new book uses
  // the SAME font as the previous one — keeps the warm bitmap LRU cache in
  // place so the first page renders instantly instead of paying the 15-30s
  // cold-load penalty for Cyrillic/CJK glyphs.
  const reader::HeapState heapBeforeTrim = reader::readHeapState();
  const size_t fontBytesBeforeTrim = FONT_MANAGER.getTotalFontMemoryUsage();
  THEME_MANAGER.clearCache();
  const Theme& currentTheme = THEME_MANAGER.current();
  const char* newReaderFontFamily = core.settings.getReaderFontFamily(currentTheme);
  const bool sameReaderFontAsBefore = FONT_MANAGER.isReaderFontAlreadyActive(newReaderFontFamily);
  if (!sameReaderFontAsBefore) {
    FONT_MANAGER.unloadReaderFonts();
    renderer_.clearWidthCache();
  }
  const reader::HeapState heapAfterTrim = reader::readHeapState();
  LOG_INF(TAG, "Entry heap trim: free=%u->%u largest=%u->%u fontBytes=%u sameFont=%u",
          static_cast<unsigned>(heapBeforeTrim.freeBytes), static_cast<unsigned>(heapAfterTrim.freeBytes),
          static_cast<unsigned>(heapBeforeTrim.largestBlock), static_cast<unsigned>(heapAfterTrim.largestBlock),
          static_cast<unsigned>(fontBytesBeforeTrim), static_cast<unsigned>(sameReaderFontAsBefore));

  contentLoaded_ = false;
  loadFailed_ = false;
  needsRender_ = true;
  navigationController_.resetSession();
  asyncJobs_.clearPendingTocJump();
  asyncJobs_.clearPendingPageLoad();
  asyncJobs_.pendingRefresh().clear();
  stopBackgroundCaching();  // Ensure any previous task is stopped
  cacheController_.resetSession();
  invalidateGlobalPageMetrics();
  currentSpineIndex_ = 0;
  currentSectionPage_ = 0;  // Will be set to -1 after progress load if at start
  pagesUntilFullRefresh_ = (core.settings.getPagesPerRefreshValue() == 0) ? 0 : 1;
  directUiTransition_ = core.pendingDirectReaderTransition;
  core.pendingDirectReaderTransition = false;
  resumeBackgroundCachingAfterRender_ = false;
  lastReaderInteractionMs_ = millis();

  // Always prefer an explicitly queued path from UI transitions.
  if (core.buf.path[0] != '\0') {
    strncpy(contentPath_, core.buf.path, sizeof(contentPath_) - 1);
    contentPath_[sizeof(contentPath_) - 1] = '\0';
    core.buf.path[0] = '\0';
    cacheController_.setContentPath(contentPath_);
  }

  // Determine source state from boot transition
  const auto& transition = getTransition();
  if (directUiTransition_) {
    sourceState_ = core.pendingReaderReturnState;
  } else {
    sourceState_ =
        (transition.isValid() && transition.returnTo == ReturnTo::FILE_MANAGER) ? StateId::FileList : StateId::Home;
  }

  LOG_INF(TAG, "Entering with path: %s", contentPath_);

  if (contentPath_[0] == '\0') {
    LOG_ERR(TAG, "No content path set");
    return;
  }

  // Apply orientation setting to renderer
  switch (core.settings.orientation) {
    case Settings::Portrait:
      renderer_.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case Settings::LandscapeCW:
      renderer_.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case Settings::Inverted:
      renderer_.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case Settings::LandscapeCCW:
      renderer_.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      renderer_.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
  }

  // Show a "Loading..." banner overlay immediately so the user gets visual
  // feedback while the (potentially multi-second) content parsing runs.
  // Uses the builtin UI font because reader streaming fonts were just unloaded.
  // Skip when waking from SleepPage — the "Sleeping" banner is already on the
  // e-ink panel over the page, so the user already has visual context.  The first
  // page render will clear the banner with drive-all.
  if (core.wokeFromSleep && core.settings.sleepScreen == Settings::SleepPage) {
    forceCleanRefreshOnNext_ = true;
  } else {
    renderCenteredStatusMessage(core, "Loading...", THEME_MANAGER.current().uiFontId);
  }

  // Open content using ContentHandle
  auto result = core.content.open(contentPath_, PAPYRIX_CACHE_DIR);
  if (!result.ok()) {
    LOG_ERR(TAG, "Failed to open content: %s", errorToString(result.err));
    // Store error message for ErrorState to display
    snprintf(core.buf.text, sizeof(core.buf.text), "Cannot open file:\n%s", errorToString(result.err));
    loadFailed_ = true;  // Mark as failed for update() to transition to error state
    return;
  }

  contentLoaded_ = true;

  // Save last book path to settings
  strncpy(core.settings.lastBookPath, contentPath_, sizeof(core.settings.lastBookPath) - 1);
  core.settings.lastBookPath[sizeof(core.settings.lastBookPath) - 1] = '\0';
  core.settings.save(core.storage);

  // Setup cache directories for all content types
  // Reset state for new book
  textStartIndex_ = 0;
  hasCover_ = false;
  thumbnailDone_ = false;
  switch (core.content.metadata().type) {
    case ContentType::Epub: {
      auto* provider = core.content.asEpub();
      if (provider && provider->getEpub()) {
        const auto* epub = provider->getEpub();
        epub->setupCacheDir();
        // Get the spine index for the first text content (from <guide> element)
        textStartIndex_ = epub->getSpineIndexForTextReference();
        LOG_DBG(TAG, "Text starts at spine index %d", textStartIndex_);
      }
      break;
    }
    case ContentType::Txt: {
      auto* provider = core.content.asTxt();
      if (provider && provider->getTxt()) {
        provider->getTxt()->setupCacheDir();
      }
      break;
    }
    case ContentType::Markdown: {
      auto* provider = core.content.asMarkdown();
      if (provider && provider->getMarkdown()) {
        provider->getMarkdown()->setupCacheDir();
      }
      break;
    }
    case ContentType::Fb2: {
      auto* provider = core.content.asFb2();
      if (provider && provider->getFb2()) {
        provider->getFb2()->setupCacheDir();
      }
      break;
    }
    case ContentType::Html: {
      auto* provider = core.content.asHtml();
      if (provider && provider->getHtml()) {
        provider->getHtml()->setupCacheDir();
      }
      break;
    }
    default:
      break;
  }

  // Load saved progress
  ContentType type = core.content.metadata().type;
  auto progress = ProgressManager::load(core, core.content.cacheDir(), type);
  progress = ProgressManager::validate(core, type, progress);
  currentSpineIndex_ = progress.spineIndex;
  currentSectionPage_ = progress.sectionPage;
  currentPage_ = progress.flatPage;

  bookmarkCount_ = BookmarkManager::load(core, core.content.cacheDir(), bookmarks_, BookmarkManager::MAX_BOOKMARKS);

  // If at start of book and showImages enabled, begin at cover
  // Skip for XTC — uses flat page indexing, no cover page concept in reader
  if (type != ContentType::Xtc && currentSpineIndex_ == 0 && currentSectionPage_ == 0 && core.settings.showImages) {
    currentSectionPage_ = -1;  // Cover page
  }

  // Initialize last rendered to loaded position (until first render)
  lastRenderedSpineIndex_ = currentSpineIndex_;
  lastRenderedSectionPage_ = currentSectionPage_;
  lastIdleBackgroundKickMs_ = 0;

  LOG_INF(TAG, "Loaded: %s", core.content.metadata().title);
  activeCore_ = &core;
  if (!asyncJobs_.startWorker()) {
    LOG_ERR(TAG, "[ASYNC] failed to start long-lived reader worker");
  }

  // Eagerly load existing page cache from disk so the first render can display
  // the page immediately (~20ms SD read) instead of deferring to the background
  // worker (~seconds for cold extend + prefetch).  The "Loading…" banner's
  // display refresh is synchronous (displayBufferDriveAll), so the SPI bus is
  // free at this point and there is no contention with the panel.
  loadCacheFromDisk(core);

  // Delay background caching until after the first reader frame is shown.
  // Display and SD share the same SPI bus on X4, so starting PageCache here
  // can race with the initial screen refresh and trip SPI transaction asserts.
}

void ReaderState::exit(Core& core) {
  LOG_INF(TAG, "Exiting");
  invalidateGlobalPageMetrics();

  // Stop background caching task first - BackgroundTask::stop() waits properly
  stopBackgroundCaching();
  asyncJobs_.stopWorker();
  asyncJobs_.clearPendingTocJump();
  asyncJobs_.clearPendingPageLoad();
  asyncJobs_.pendingRefresh().clear();

  if (contentLoaded_) {
    // Save progress at last rendered position (not current requested position)
    ProgressManager::Progress progress;
    // If on cover, save as (0, 0) - cover is implicit start
    progress.spineIndex = (lastRenderedSectionPage_ == -1) ? 0 : lastRenderedSpineIndex_;
    progress.sectionPage = (lastRenderedSectionPage_ == -1) ? 0 : lastRenderedSectionPage_;
    progress.flatPage = currentPage_;
    ProgressManager::save(core, core.content.cacheDir(), core.content.metadata().type, progress);
    saveBookmarks(core);

    // Safe to reset - task is stopped, we own pageCache_/parser_
    cacheController_.resetSession();
    core.content.close();
  }

  // Keep the active .epdfont reader family loaded across UI transitions.
  // Several UI surfaces reuse theme.readerFontId directly, so unloading the
  // family here leaves stale font IDs in the renderer path and causes
  // "Font <id> not found" logs after leaving the reader. The large external
  // CJK fallback font is still safe to release.
  FONT_MANAGER.unloadExternalFont();

  // Reset overlay modes that may have been active when exit was triggered
  menuMode_ = false;
  bookmarkMode_ = false;
  tocMode_ = false;

  contentLoaded_ = false;
  contentPath_[0] = '\0';
  cacheController_.setContentPath(contentPath_);
  activeCore_ = nullptr;
  directUiTransition_ = false;
  resumeBackgroundCachingAfterRender_ = false;
  lastIdleBackgroundKickMs_ = 0;
  pendingTocJumpActive_ = false;
  pendingTocJumpIndexingShown_ = false;
  pendingTocJumpTargetSpine_ = -1;
  pendingTocJumpTargetPageHint_ = -1;
  pendingTocJumpAnchor_.clear();
  pendingTocJumpRetryCount_ = 0;
  clearPendingEpubPageLoad();
  invalidateAnchorMapCache();

  // Reset orientation to Portrait for UI
  renderer_.setOrientation(GfxRenderer::Orientation::Portrait);
}

StateTransition ReaderState::update(Core& core) {
  // Handle load failure - transition to error state or back to file list
  if (loadFailed_ || !contentLoaded_) {
    // If error message was set, show ErrorState; otherwise just go back to FileList
    if (core.buf.text[0] != '\0') {
      return StateTransition::to(StateId::Error);
    }
    return StateTransition::to(StateId::FileList);
  }

  if (pendingTocJumpActive_ && !needsRender_) {
    processPendingTocJump(core);
    return StateTransition::stay(StateId::Reader);
  }

  if (pendingEpubPageLoadActive_ && !needsRender_) {
    processPendingEpubPageLoad(core);
    Event pendingEvent;
    while (core.events.pop(pendingEvent)) {
      if (pendingEvent.type == EventType::ButtonPress || pendingEvent.type == EventType::ButtonRelease ||
          pendingEvent.type == EventType::ButtonRepeat) {
        lastReaderInteractionMs_ = millis();
      }
      if (pendingEvent.type == EventType::ButtonPress && pendingEvent.button == Button::Back) {
        return exitToUI(core);
      }

      if (pendingEvent.type == EventType::ButtonPress && pendingEvent.button == Button::Power &&
          core.settings.shortPwrBtn == Settings::PowerPageTurn) {
        powerPressStartedMs_ = millis();
        continue;
      }

      if (pendingEvent.type != EventType::ButtonRelease) {
        continue;
      }

      switch (pendingEvent.button) {
        case Button::Right:
        case Button::Down:
          enqueuePendingPageTurn(1, "pending-epub-page-load");
          break;
        case Button::Left:
        case Button::Up:
          enqueuePendingPageTurn(-1, "pending-epub-page-load");
          break;
        case Button::Power:
          if (core.settings.shortPwrBtn == Settings::PowerPageTurn && powerPressStartedMs_ != 0) {
            const uint32_t heldMs = millis() - powerPressStartedMs_;
            if (heldMs < core.settings.getPowerButtonDuration()) {
              enqueuePendingPageTurn(1, "pending-epub-page-load");
            }
          }
          powerPressStartedMs_ = 0;
          break;
        default:
          break;
      }
    }
    return StateTransition::stay(StateId::Reader);
  }

  if (queuedPendingEpubTurn_ != 0 && !needsRender_ && !pendingTocJumpActive_ && !pendingEpubPageLoadActive_ &&
      !menuMode_ && !bookmarkMode_ && !tocMode_) {
    int queuedTurn = 0;
    uint32_t queuedForMs = 0;
    navigationController_.noteWorkerIdle(isWorkerRunning());
    if (navigationController_.tryConsumeQueuedTurn(isWorkerRunning(), needsRender_, pendingTocJumpActive_,
                                                   pendingEpubPageLoadActive_, menuMode_, bookmarkMode_, tocMode_,
                                                   queuedTurn, queuedForMs)) {
      LOG_INF(TAG, "[INPUT] executing deferred page-turn dir=%d wait=%lu remaining=%d", queuedTurn,
              static_cast<unsigned long>(queuedForMs), queuedPendingEpubTurn_);

      if (queuedTurn > 0) {
        navigateNext(core);
      } else {
        navigatePrev(core);
      }
      return StateTransition::stay(StateId::Reader);
    }
  }

  if (pendingBackgroundEpubRefresh_) {
    if (currentSpineIndex_ != pendingBackgroundEpubRefreshSpine_ ||
        currentSectionPage_ != pendingBackgroundEpubRefreshPage_) {
      pendingBackgroundEpubRefresh_ = false;
      pendingBackgroundEpubRefreshSpine_ = -1;
      pendingBackgroundEpubRefreshPage_ = -1;
    } else if (!needsRender_ && !pendingTocJumpActive_ && !pendingEpubPageLoadActive_ && !menuMode_ &&
               !bookmarkMode_ && !tocMode_) {
      // Don't gate on !isWorkerRunning(): after wake from deep sleep the worker
      // loads the current cache from disk (~20ms) then continues with extend +
      // prefetch (~seconds).  If we wait for the worker to fully finish, the
      // page won't render until the prefetch completes.  The cache file is
      // already written; a concurrent worker poses no risk to the reader.
      if (isWorkerRunning()) {
        stopBackgroundCaching();
        resumeBackgroundCachingAfterRender_ = true;
      }
      LOG_INF(TAG, "[CACHE] refreshing current page after background cache rewrite spine=%d page=%d",
              currentSpineIndex_, currentSectionPage_);
      pendingBackgroundEpubRefresh_ = false;
      pendingBackgroundEpubRefreshSpine_ = -1;
      pendingBackgroundEpubRefreshPage_ = -1;
      needsRender_ = true;
    }
  }

  Event e;
  while (core.events.pop(e)) {
    if (e.type == EventType::ButtonPress || e.type == EventType::ButtonRelease || e.type == EventType::ButtonRepeat) {
      lastReaderInteractionMs_ = millis();
    }
    if (menuMode_) {
      handleMenuInput(core, e);
      continue;
    }
    if (bookmarkMode_) {
      handleBookmarkInput(core, e);
      continue;
    }
    if (tocMode_) {
      handleTocInput(core, e);
      continue;
    }

    switch (e.type) {
      case EventType::ButtonPress:
        switch (e.button) {
          case Button::Right:
          case Button::Down:
          case Button::Left:
          case Button::Up:
            if (isWorkerRunning()) {
              lastCachePreemptRequestedMs_ = millis();
              LOG_INF(TAG, "[INPUT] preempt requested button=%d workerState=%d", static_cast<int>(e.button),
                      static_cast<int>(workerState()));
              requestWorkerCancel();
            }
            break;
          case Button::Center:
            enterMenuMode(core);
            break;
          case Button::Back:
            return exitToUI(core);
          case Button::Power:
            if (core.settings.shortPwrBtn == Settings::PowerPageTurn) {
              powerPressStartedMs_ = millis();
              if (isWorkerRunning()) {
                lastCachePreemptRequestedMs_ = millis();
                LOG_INF(TAG, "[INPUT] preempt requested button=%d workerState=%d", static_cast<int>(e.button),
                        static_cast<int>(workerState()));
                requestWorkerCancel();
              }
            }
            break;
          default:
            break;
        }
        break;

      case EventType::ButtonRepeat:
        if (!holdNavigated_) {
          switch (e.button) {
            case Button::Right:
            case Button::Down:
              navigateNextChapter(core);
              holdNavigated_ = true;
              break;
            case Button::Left:
            case Button::Up:
              navigatePrevChapter(core);
              holdNavigated_ = true;
              break;
            default:
              break;
          }
        }
        break;

      case EventType::ButtonRelease:
        if (!holdNavigated_) {
          switch (e.button) {
            case Button::Right:
            case Button::Down:
              LOG_INF(TAG, "[INPUT] page-turn release button=%d workerActive=%u", static_cast<int>(e.button),
                      static_cast<unsigned>(isWorkerRunning()));
              navigateNext(core);
              break;
            case Button::Left:
            case Button::Up:
              LOG_INF(TAG, "[INPUT] page-turn release button=%d workerActive=%u", static_cast<int>(e.button),
                      static_cast<unsigned>(isWorkerRunning()));
              navigatePrev(core);
              break;
            case Button::Power:
              if (core.settings.shortPwrBtn == Settings::PowerPageTurn && powerPressStartedMs_ != 0) {
                const uint32_t heldMs = millis() - powerPressStartedMs_;
                if (heldMs < core.settings.getPowerButtonDuration()) {
                  LOG_INF(TAG, "[INPUT] page-turn release button=%d workerActive=%u", static_cast<int>(e.button),
                          static_cast<unsigned>(isWorkerRunning()));
                  navigateNext(core);
                }
              }
              break;
            default:
              break;
          }
        }
        if (e.button == Button::Power) {
          powerPressStartedMs_ = 0;
        }
        holdNavigated_ = false;
        break;

      default:
        break;
    }
  }

  if (!needsRender_ && !isWorkerRunning() && !pendingTocJumpActive_ && !pendingEpubPageLoadActive_ && !menuMode_ &&
      !bookmarkMode_ && !tocMode_) {
    const uint32_t nowMs = millis();
    if (lastIdleBackgroundKickMs_ == 0 || nowMs - lastIdleBackgroundKickMs_ >= kIdleBackgroundKickIntervalMs) {
      lastIdleBackgroundKickMs_ = nowMs;
      startBackgroundCaching(core, "idle");
    }
  }

  return StateTransition::stay(StateId::Reader);
}

void ReaderState::render(Core& core) {
  if (!needsRender_ || !contentLoaded_) {
    return;
  }

  const bool overlayRender = menuMode_ || bookmarkMode_ || tocMode_;
  if (overlayRender && isWorkerRunning()) {
    stopBackgroundCaching();
  }

  if (pendingTocJumpActive_) {
    if (!pendingTocJumpIndexingShown_) {
      renderCenteredStatusMessage(core, "Indexing...");
      pendingTocJumpIndexingShown_ = true;
    }
  } else if (pendingEpubPageLoadActive_) {
    if (!pendingEpubPageLoadMessageShown_) {
      renderCenteredStatusMessage(core, pendingEpubPageLoadUseIndexingMessage_ ? "Indexing..." : "Loading...");
      pendingEpubPageLoadMessageShown_ = true;
    }
  } else if (menuMode_) {
    const Theme& theme = THEME_MANAGER.current();
    ui::render(renderer_, theme, menuView_);
    core.display.markDirty();
  } else if (bookmarkMode_) {
    renderBookmarkOverlay(core);
  } else if (tocMode_) {
    renderTocOverlay(core);
  } else {
    renderCurrentPage(core);
    if (!pendingEpubPageLoadActive_) {
      lastRenderedSpineIndex_ = currentSpineIndex_;
      lastRenderedSectionPage_ = currentSectionPage_;
    }
    if (resumeBackgroundCachingAfterRender_) {
      resumeBackgroundCachingAfterRender_ = false;
      if (!isWorkerRunning()) {
        startBackgroundCaching(core, "resume-after-render");
      }
    }
  }

  needsRender_ = false;
}

void ReaderState::navigateNext(Core& core) {
  ContentType type = core.content.metadata().type;

  // XTC uses flatPage navigation, not spine/section - skip to navigation logic
  if (type == ContentType::Xtc) {
    stopBackgroundCaching();
    ReaderNavigation::Position pos;
    pos.flatPage = currentPage_;
    auto result = ReaderNavigation::next(type, pos, nullptr, core.content.pageCount());
    applyNavResult(result, core);
    return;
  }

  if (deferPageTurnUntilCacheStops(1)) {
    return;
  }

  if (tryFastNavigateNext(core)) {
    return;
  }

  // Spine/section logic for EPUB, TXT, Markdown
  // From cover (-1) -> first text content page
  if (currentSpineIndex_ == 0 && currentSectionPage_ == -1) {
    stopBackgroundCaching();
    auto* provider = core.content.asEpub();
    size_t spineCount = 1;
    if (provider && provider->getEpub()) {
      spineCount = provider->getEpub()->getSpineItemsCount();
    }
    int firstContentSpine = calcFirstContentSpine(hasCover_, textStartIndex_, spineCount);

    if (firstContentSpine != currentSpineIndex_) {
      currentSpineIndex_ = firstContentSpine;
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
    currentSectionPage_ = 0;
    needsRender_ = true;
    resumeBackgroundCachingAfterRender_ = false;
    return;
  }

  if (tryFastNavigateWithinCurrentCache(core, 1)) {
    return;
  }

  stopBackgroundCaching();

  if (type != ContentType::Epub && pageCache_ && pageCache_->pageCount() == 0) {
    const int targetPage = std::max(currentSectionPage_ + 1, 0);
    LOG_INF(TAG, "[NAV] next recovering from empty active cache spine=%d targetPage=%d", currentSpineIndex_,
            targetPage);
    pageCache_.reset();
    invalidateAnchorMapCache();
    clearPagePrefetch();
    armPendingEpubPageLoad(core, currentSpineIndex_, targetPage, false, false);
    needsRender_ = true;
    resumeBackgroundCachingAfterRender_ = false;
    return;
  }

  ReaderNavigation::Position pos;
  pos.spineIndex = currentSpineIndex_;
  pos.sectionPage = currentSectionPage_;
  pos.flatPage = currentPage_;
  const uint32_t navTotal = (type == ContentType::Fb2) ? core.content.tocCount() : core.content.pageCount();
  auto result = ReaderNavigation::next(type, pos, pageCache_.get(), navTotal);
  if (!result.needsRender && result.position.spineIndex == pos.spineIndex && result.position.sectionPage == pos.sectionPage &&
      result.position.flatPage == pos.flatPage) {
    LOG_DBG(TAG, "[NAV] next no-op type=%d spine=%d page=%d cache=%u pages=%u partial=%u", static_cast<int>(type),
            currentSpineIndex_, currentSectionPage_, static_cast<unsigned>(pageCache_ != nullptr),
            static_cast<unsigned>(pageCache_ ? pageCache_->pageCount() : 0),
            static_cast<unsigned>(pageCache_ ? pageCache_->isPartial() : 0));
  }
  applyNavResult(result, core);
}

bool ReaderState::tryFastNavigateNext(Core& core) {
  (void)core;

  if (currentSectionPage_ < 0) {
    return false;
  }

  const int targetPage = currentSectionPage_ + 1;
  if (!warmedNextPage_.matches(currentSpineIndex_, targetPage)) {
    return false;
  }

  renderOverridePage_ = warmedNextPage_;
  warmedNextPage_ = warmedNextNextPage_;
  warmedNextNextPage_.clear();

  currentSectionPage_ = targetPage;
  clearPendingEpubPageLoad();
  needsRender_ = true;
  resumeBackgroundCachingAfterRender_ = false;

  LOG_DBG(TAG, "Fast next-page turn using detached warm page spine=%d page=%d", currentSpineIndex_, currentSectionPage_);
  return true;
}

bool ReaderState::tryFastNavigateWithinCurrentCache(Core& core, const int direction) {
  (void)core;

  if (isWorkerRunning() || !pageCache_ || pageCache_->isPartial()) {
    return false;
  }

  const int targetPage = currentSectionPage_ + direction;
  const int pageCount = static_cast<int>(pageCache_->pageCount());
  if (targetPage < 0 || targetPage >= pageCount) {
    return false;
  }

  currentSectionPage_ = targetPage;
  clearPendingEpubPageLoad();
  needsRender_ = true;
  resumeBackgroundCachingAfterRender_ = false;

  LOG_DBG(TAG, "Fast cached page turn using active cache spine=%d page=%d dir=%d", currentSpineIndex_,
          currentSectionPage_, direction);
  return true;
}

void ReaderState::navigatePrev(Core& core) {
  ContentType type = core.content.metadata().type;

  // XTC uses flatPage navigation, not spine/section - skip to navigation logic
  if (type == ContentType::Xtc) {
    stopBackgroundCaching();
    ReaderNavigation::Position pos;
    pos.flatPage = currentPage_;
    auto result = ReaderNavigation::prev(type, pos, nullptr);
    applyNavResult(result, core);
    return;
  }

  // Spine/section logic for EPUB, TXT, Markdown
  auto* provider = core.content.asEpub();
  size_t spineCount = 1;
  if (provider && provider->getEpub()) {
    spineCount = provider->getEpub()->getSpineItemsCount();
  }
  int firstContentSpine = calcFirstContentSpine(hasCover_, textStartIndex_, spineCount);

  // Prevent going back from cover
  if (currentSpineIndex_ == 0 && currentSectionPage_ == -1) {
    startBackgroundCaching(core, "nav-prev-cover");  // Resume task before returning
    return;                        // Already at cover
  }

  if (deferPageTurnUntilCacheStops(-1)) {
    return;
  }

  // At first page of text content
  if (currentSpineIndex_ == firstContentSpine && currentSectionPage_ == 0) {
    // Only go to cover if it exists and images enabled
    if (hasCover_ && core.settings.showImages) {
      stopBackgroundCaching();
      currentSpineIndex_ = 0;
      currentSectionPage_ = -1;
      parser_.reset();
      parserSpineIndex_ = -1;
      clearLookaheadParser();
      pageCache_.reset();  // Don't need cache for cover
      invalidateAnchorMapCache();
      clearPagePrefetch();
      resetBackgroundPrefetchState();
      needsRender_ = true;
    }
    return;  // At start of book either way
  }

  if (tryFastNavigateWithinCurrentCache(core, -1)) {
    return;
  }

  stopBackgroundCaching();

  if (type != ContentType::Epub && pageCache_ && pageCache_->pageCount() == 0 && currentSectionPage_ > 0) {
    const int targetPage = std::max(currentSectionPage_ - 1, 0);
    LOG_INF(TAG, "[NAV] prev recovering from empty active cache spine=%d targetPage=%d", currentSpineIndex_,
            targetPage);
    pageCache_.reset();
    invalidateAnchorMapCache();
    clearPagePrefetch();
    armPendingEpubPageLoad(core, currentSpineIndex_, targetPage, false, false);
    needsRender_ = true;
    resumeBackgroundCachingAfterRender_ = false;
    return;
  }

  ReaderNavigation::Position pos;
  pos.spineIndex = currentSpineIndex_;
  pos.sectionPage = currentSectionPage_;
  pos.flatPage = currentPage_;
  auto result = ReaderNavigation::prev(type, pos, pageCache_.get());
  if (!result.needsRender && result.position.spineIndex == pos.spineIndex && result.position.sectionPage == pos.sectionPage &&
      result.position.flatPage == pos.flatPage) {
    LOG_DBG(TAG, "[NAV] prev no-op type=%d spine=%d page=%d cache=%u pages=%u partial=%u", static_cast<int>(type),
            currentSpineIndex_, currentSectionPage_, static_cast<unsigned>(pageCache_ != nullptr),
            static_cast<unsigned>(pageCache_ ? pageCache_->pageCount() : 0),
            static_cast<unsigned>(pageCache_ ? pageCache_->isPartial() : 0));
  }
  applyNavResult(result, core);
}

void ReaderState::applyNavResult(const ReaderNavigation::NavResult& result, Core& core) {
  const int previousSpineIndex = currentSpineIndex_;
  currentSpineIndex_ = result.position.spineIndex;
  currentSectionPage_ = result.position.sectionPage;
  currentPage_ = result.position.flatPage;
  if (core.content.metadata().type == ContentType::Fb2) {
    currentPage_ = currentSectionPage_;
  }
  // Use |= so a no-op navigation (needsRender=false) doesn't cancel a
  // pre-existing render request.  This is critical on wake from deep sleep:
  // enter() sets needsRender_=true, but the wake button can fire a spurious
  // page-turn that produces a no-op NavResult before the first render runs.
  needsRender_ |= result.needsRender;
  clearPendingEpubPageLoad();
  if (result.needsCacheReset) {
    parser_.reset();  // Safe - task already stopped by caller
    parserSpineIndex_ = -1;
    if (lookaheadParserSpineIndex_ != currentSpineIndex_) {
      clearLookaheadParser();
    }
    pageCache_.reset();
    invalidateAnchorMapCache();
    clearPagePrefetch();
    resetBackgroundPrefetchState();
  } else if (currentSpineIndex_ != previousSpineIndex) {
    resetBackgroundPrefetchState();
  }
  if (result.needsRender) {
    resumeBackgroundCachingAfterRender_ = false;
  } else {
    startBackgroundCaching(core, "nav-no-render");  // Resume caching when no visible page render is pending
  }
}

void ReaderState::navigateNextChapter(Core& core) {
  ContentType type = core.content.metadata().type;

  if (type == ContentType::Xtc) {
    const uint16_t count = core.content.tocCount();
    if (count == 0) return;

    // Find current chapter
    int currentChapter = -1;
    for (uint16_t i = 0; i < count; i++) {
      auto result = core.content.getTocEntry(i);
      if (result.ok() && result.value.pageIndex <= currentPage_) {
        currentChapter = i;
      }
    }

    if (currentChapter + 1 >= static_cast<int>(count)) return;

    auto next = core.content.getTocEntry(currentChapter + 1);
    if (!next.ok()) return;

    currentPage_ = next.value.pageIndex;
    needsRender_ = true;
    return;
  }

  if (type == ContentType::Fb2) {
    const uint16_t count = core.content.tocCount();
    if (count == 0 || currentSpineIndex_ + 1 >= static_cast<int>(count)) return;

    stopBackgroundCaching();
    currentSpineIndex_++;
    currentSectionPage_ = 0;
    parser_.reset();
    parserSpineIndex_ = -1;
    clearLookaheadParser();
    pageCache_.reset();
    invalidateAnchorMapCache();
    clearPagePrefetch();
    resetBackgroundPrefetchState();
    needsRender_ = true;
    resumeBackgroundCachingAfterRender_ = false;
    return;
  }

  if (type != ContentType::Epub) return;

  auto* provider = core.content.asEpub();
  if (!provider || !provider->getEpub()) return;

  size_t spineCount = provider->getEpub()->getSpineItemsCount();
  if (currentSpineIndex_ + 1 >= static_cast<int>(spineCount)) return;

  stopBackgroundCaching();
  currentSpineIndex_++;
  currentSectionPage_ = 0;
  parser_.reset();
  parserSpineIndex_ = -1;
  if (lookaheadParserSpineIndex_ != currentSpineIndex_) {
    clearLookaheadParser();
  }
  pageCache_.reset();
  invalidateAnchorMapCache();
  clearPagePrefetch();
  resetBackgroundPrefetchState();
  needsRender_ = true;
  resumeBackgroundCachingAfterRender_ = false;
}

void ReaderState::navigatePrevChapter(Core& core) {
  ContentType type = core.content.metadata().type;

  if (type == ContentType::Xtc) {
    const uint16_t count = core.content.tocCount();
    if (count == 0) return;

    // Find current chapter
    int currentChapter = -1;
    uint32_t currentChapterStart = 0;
    for (uint16_t i = 0; i < count; i++) {
      auto result = core.content.getTocEntry(i);
      if (result.ok() && result.value.pageIndex <= currentPage_) {
        currentChapter = i;
        currentChapterStart = result.value.pageIndex;
      }
    }

    if (currentChapter < 0) return;

    if (currentPage_ > currentChapterStart) {
      // Mid-chapter: go to start of current chapter
      currentPage_ = currentChapterStart;
    } else if (currentChapter > 0) {
      // At start of chapter: go to previous chapter
      auto prev = core.content.getTocEntry(currentChapter - 1);
      if (!prev.ok()) return;
      currentPage_ = prev.value.pageIndex;
    } else {
      return;
    }

    needsRender_ = true;
    return;
  }

  if (type == ContentType::Fb2) {
    stopBackgroundCaching();

    if (currentSectionPage_ > 0) {
      currentSectionPage_ = 0;
    } else {
      if (currentSpineIndex_ <= 0) {
        startBackgroundCaching(core, "fb2-prev-chapter-boundary");
        return;
      }
      currentSpineIndex_--;
      currentSectionPage_ = 0;
      parser_.reset();
      parserSpineIndex_ = -1;
      clearLookaheadParser();
      pageCache_.reset();
      invalidateAnchorMapCache();
      clearPagePrefetch();
      resetBackgroundPrefetchState();
    }

    needsRender_ = true;
    resumeBackgroundCachingAfterRender_ = false;
    return;
  }

  if (type != ContentType::Epub) return;

  stopBackgroundCaching();

  if (currentSectionPage_ > 0) {
    // Go to beginning of current chapter
    currentSectionPage_ = 0;
  } else {
    // Go to previous chapter
    auto* provider = core.content.asEpub();
    size_t spineCount = 1;
    if (provider && provider->getEpub()) {
      spineCount = provider->getEpub()->getSpineItemsCount();
    }
    int firstContentSpine = calcFirstContentSpine(hasCover_, textStartIndex_, spineCount);
    if (currentSpineIndex_ <= firstContentSpine) {
      startBackgroundCaching(core, "prev-chapter-boundary");
      return;
    }
    currentSpineIndex_--;
    currentSectionPage_ = 0;
    parser_.reset();
    parserSpineIndex_ = -1;
    clearLookaheadParser();
    pageCache_.reset();
    invalidateAnchorMapCache();
    clearPagePrefetch();
    resetBackgroundPrefetchState();
  }

  needsRender_ = true;
  resumeBackgroundCachingAfterRender_ = false;
}

void ReaderState::renderCurrentPage(Core& core) {
  ContentType type = core.content.metadata().type;
  const Theme& theme = THEME_MANAGER.current();

  if (type == ContentType::Epub && !pendingTocJumpActive_ && pendingTocJumpRetryCount_ > 0) {
    crashdebug::mark(crashdebug::CrashPhase::EpubTocRender, static_cast<int16_t>(currentSpineIndex_),
                     pendingTocJumpRetryCount_);
  }

  // Always clear screen first (prevents previous content from showing through)
  renderer_.clearScreen(theme.backgroundColor);

  // Cover page: spineIndex=0, sectionPage=-1 (only when showImages enabled)
  if (currentSpineIndex_ == 0 && currentSectionPage_ == -1) {
    if (core.settings.showImages) {
      if (renderCoverPage(core)) {
        hasCover_ = true;
        core.display.markDirty();
        return;
      }
      // No cover - skip spine 0 if textStartIndex is 0 (likely empty cover document)
      hasCover_ = false;
      currentSectionPage_ = 0;
      if (textStartIndex_ == 0) {
        // Only skip to spine 1 if it exists
        auto* provider = core.content.asEpub();
        if (provider && provider->getEpub()) {
          const auto* epub = provider->getEpub();
          if (epub->getSpineItemsCount() > 1) {
            currentSpineIndex_ = 1;
          }
        }
      }
      // Fall through to render content
    } else {
      currentSectionPage_ = 0;
    }
  }

  switch (type) {
    case ContentType::Epub:
    case ContentType::Txt:
    case ContentType::Markdown:
    case ContentType::Fb2:
    case ContentType::Html:
      renderCachedPage(core);
      break;
    case ContentType::Xtc:
      renderXtcPage(core);
      break;
    default:
      break;
  }

  if (!pendingTocJumpActive_ && !pendingEpubPageLoadActive_ && !isWorkerRunning()) {
    startBackgroundCaching(core, "post-render");
  }

  if (type == ContentType::Epub && !pendingTocJumpActive_ && pendingTocJumpRetryCount_ > 0) {
    crashdebug::clear();
    pendingTocJumpRetryCount_ = 0;
  }

  core.display.markDirty();
}

void ReaderState::renderCachedPage(Core& core) {
  const uint32_t totalStartMs = reader::perfMsNow();
  Theme& theme = THEME_MANAGER.mutableCurrent();
  ContentType type = core.content.metadata().type;
  const auto vp = getReaderViewport(core.settings.statusBar != 0);

  // Handle EPUB bounds
  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return;

    auto epub = provider->getEpubShared();
    if (currentSpineIndex_ < 0) currentSpineIndex_ = 0;
    if (currentSpineIndex_ >= static_cast<int>(epub->getSpineItemsCount())) {
      renderer_.drawCenteredText(core.settings.getReaderFontId(theme), 300, "End of book", theme.primaryTextBlack,
                                 BOLD);
      renderer_.displayBuffer();
      return;
    }
  } else if (type == ContentType::Fb2) {
    auto* provider = core.content.asFb2();
    if (fb2UsesSectionNavigation(provider)) {
      const int tocCount = static_cast<int>(provider->tocCount());
      if (tocCount <= 0) {
        return;
      }
      if (currentSpineIndex_ < 0) {
        currentSpineIndex_ = 0;
      } else if (currentSpineIndex_ >= tocCount) {
        currentSpineIndex_ = tocCount - 1;
        currentSectionPage_ = 0;
      }
    }
  }

  if (renderOverridePage_.matches(currentSpineIndex_, currentSectionPage_)) {
    if (isWorkerRunning()) {
      LOG_INF(TAG, "[ASYNC] forcing worker stop before detached warm render state=%d", static_cast<int>(workerState()));
      stopBackgroundCaching();
    }
    WarmPageSlot pageSlot = renderOverridePage_;
    renderOverridePage_.clear();
    LOG_DBG(TAG, "Rendered via detached warm page spine=%d page=%d", currentSpineIndex_, currentSectionPage_);
    renderLoadedPage(core, pageSlot.page, pageSlot.pageCount, pageSlot.isPartial, theme, vp, totalStartMs,
                     !isWorkerRunning());
    return;
  }

  if (isWorkerRunning()) {
    stopBackgroundCaching();
  }

  // Background task may have left parser in inconsistent state
  if (!pageCache_ && parser_ && parserSpineIndex_ == currentSpineIndex_ && !parser_->canResume()) {
    parser_.reset();
    parserSpineIndex_ = -1;
  }

  // Create or load cache if needed
  if (!pageCache_) {
    const uint32_t cacheBootstrapMs = reader::perfMsNow();
    // Try to load existing cache silently first
    loadCacheFromDisk(core);

    if (pageCache_ && currentSectionPage_ == INT16_MAX && !pageCache_->isPartial() && pageCache_->pageCount() > 0) {
      currentSectionPage_ = static_cast<int>(pageCache_->pageCount()) - 1;
    }

    bool pageIsCached =
        pageCache_ && currentSectionPage_ >= 0 && currentSectionPage_ < static_cast<int>(pageCache_->pageCount());

    if (!pageIsCached) {
      const bool requireComplete = currentSectionPage_ == INT16_MAX;
      const int targetPage = requireComplete ? 0 : std::max(currentSectionPage_, 0);
      LOG_INF(TAG, "[ASYNC] deferring cache build spine=%d page=%d complete=%u type=%d", currentSpineIndex_,
              targetPage, static_cast<unsigned>(requireComplete), static_cast<int>(type));
      armPendingEpubPageLoad(core, currentSpineIndex_, targetPage, requireComplete, true);
      return;
    }
    readerPerfLog("reader-cache-bootstrap", cacheBootstrapMs, "(spine=%d page=%d)", currentSpineIndex_, currentSectionPage_);

    // Re-validate page against current cache state.  A background cold extend
    // may have replaced the cache file between the pageIsCached check above
    // and this point (a UART yield inside readerPerfLog is the window).
    // If the page is now out-of-range, treat it as uncached and defer rather
    // than silently clamping (which would jump the user backward).
    if (pageCache_) {
      const int cachedPages = static_cast<int>(pageCache_->pageCount());
      if (currentSectionPage_ < 0) {
        currentSectionPage_ = 0;
      } else if (currentSectionPage_ >= cachedPages) {
        LOG_INF(TAG, "[NAV] page out-of-range after cache change spine=%d page=%d cachedPages=%d partial=%u",
                currentSpineIndex_, currentSectionPage_, cachedPages,
                static_cast<unsigned>(pageCache_->isPartial()));
        if (pageCache_->isPartial()) {
          // Cache is still growing — request the missing page instead of clamping.
          armPendingEpubPageLoad(core, currentSpineIndex_, currentSectionPage_, false, false);
          return;
        }
        // Cache is complete.  The user's page truly doesn't exist
        // (section shrank after a rebuild).  Move to the last valid page.
        currentSectionPage_ = cachedPages > 0 ? cachedPages - 1 : 0;
      }
    }
  }

  // Check if we need to extend cache
  if (!ensurePageCached(core, currentSectionPage_)) {
    if (pendingEpubPageLoadActive_) {
      return;
    }
    LOG_ERR(TAG, "Failed to load page spine=%d page=%d cache=%u pages=%u partial=%u", currentSpineIndex_,
            currentSectionPage_, static_cast<unsigned>(pageCache_ != nullptr),
            static_cast<unsigned>(pageCache_ ? pageCache_->pageCount() : 0),
            static_cast<unsigned>(pageCache_ ? pageCache_->isPartial() : 0));
    renderer_.drawCenteredText(core.settings.getReaderFontId(theme), 300, "Failed to load page", theme.primaryTextBlack,
                               BOLD);
    renderer_.displayBuffer();
    needsRender_ = false;  // Prevent infinite render loop on cache failure
    return;
  }

  // Load and render page (cache is now guaranteed to exist, we own it)
  size_t pageCount = pageCache_ ? pageCache_->pageCount() : 0;
  const uint32_t pageLoadMs = reader::perfMsNow();
  std::shared_ptr<Page> page = pageCache_ ? pageCache_->loadPage(currentSectionPage_) : nullptr;
  readerPerfLog("reader-page-load", pageLoadMs, "(spine=%d page=%d)", currentSpineIndex_, currentSectionPage_);

  if (!page) {
    LOG_ERR(TAG, "Failed to load page, clearing cache and recovering dir structure");
    if (pageCache_) {
      pageCache_->clear();
      pageCache_.reset();
    }
    // Re-create cache directory hierarchy — SdFat can lose directory entries
    // under memory pressure, causing all subsequent cache operations to fail.
    if (core.content.metadata().type == ContentType::Epub) {
      auto* provider = core.content.asEpub();
      if (provider && provider->getEpub()) {
        provider->getEpub()->setupCacheDir();
      }
    } else if (core.content.metadata().type == ContentType::Fb2) {
      auto* provider = core.content.asFb2();
      if (provider && provider->getFb2()) {
        provider->getFb2()->setupCacheDir();
      }
    }
    invalidateAnchorMapCache();
    clearPagePrefetch();
    needsRender_ = true;
    return;
  }

  renderLoadedPage(core, page, pageCount, pageCache_ ? pageCache_->isPartial() : false, theme, vp, totalStartMs, true);
}

void ReaderState::renderLoadedPage(Core& core, const std::shared_ptr<Page>& page, const size_t pageCount,
                                   const bool cacheIsPartial, const Theme& theme, const Viewport& vp,
                                   const uint32_t totalStartMs, const bool allowPagePrefetch) {
  renderer_.clearScreen(theme.backgroundColor);

  const int fontId = core.settings.getReaderFontId(theme);
  const bool aaEnabled = core.settings.textAntiAliasing && renderer_.fontSupportsGrayscale(fontId);

  const uint32_t renderBwMs = reader::perfMsNow();
  renderPageContents(core, *page, vp.marginTop, vp.marginRight, vp.marginBottom, vp.marginLeft);
  renderStatusBar(core, vp.marginRight, vp.marginBottom, vp.marginLeft, static_cast<int>(pageCount), cacheIsPartial);
  readerPerfLog("reader-render-bw", renderBwMs, "(aa=%u images=%u)", static_cast<unsigned>(aaEnabled),
          static_cast<unsigned>(page->hasImages()));

  const uint32_t displayMs = reader::perfMsNow();
  displayWithRefresh(core);
  readerPerfLog("reader-display-main", displayMs, nullptr);

  // Grayscale text rendering (anti-aliasing)
  if (aaEnabled) {
    const uint32_t aaMs = reader::perfMsNow();
    renderer_.clearScreen(0x00);
    renderer_.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer_, fontId, vp.marginLeft, vp.marginTop, theme.primaryTextBlack);
    renderStatusBar(core, vp.marginRight, vp.marginBottom, vp.marginLeft, static_cast<int>(pageCount),
                    cacheIsPartial);
    renderer_.copyGrayscaleLsbBuffers();

    renderer_.clearScreen(0x00);
    renderer_.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer_, fontId, vp.marginLeft, vp.marginTop, theme.primaryTextBlack);
    renderStatusBar(core, vp.marginRight, vp.marginBottom, vp.marginLeft, static_cast<int>(pageCount),
                    cacheIsPartial);
    renderer_.copyGrayscaleMsbBuffers();

    const bool turnOffScreen = core.settings.sunlightFadingFix != 0;
    renderer_.displayGrayBuffer(turnOffScreen);
    renderer_.setRenderMode(GfxRenderer::BW);

    // Re-render BW instead of restoring from backup (saves 48KB peak allocation)
    renderer_.clearScreen(theme.backgroundColor);
    renderPageContents(core, *page, vp.marginTop, vp.marginRight, vp.marginBottom, vp.marginLeft);
    renderStatusBar(core, vp.marginRight, vp.marginBottom, vp.marginLeft, static_cast<int>(pageCount),
                    cacheIsPartial);
    renderer_.cleanupGrayscaleWithFrameBuffer();
    readerPerfLog("reader-aa-pass", aaMs, nullptr);
  }

  const uint32_t prefetchMs = reader::perfMsNow();
  if (allowPagePrefetch) {
    prefetchAdjacentPage(core);
    readerPerfLog("reader-prefetch", prefetchMs, nullptr);
  } else {
    readerPerfLog("reader-prefetch-skip", prefetchMs, "(reason=detached-fast-turn)");
  }

  LOG_DBG(TAG, "Rendered page %d/%d", currentSectionPage_ + 1, pageCount);
  readerPerfLog("reader-total", totalStartMs, "(spine=%d page=%d)", currentSpineIndex_, currentSectionPage_);
  // Count actual visible page presentation as reader activity so far-idle
  // sweeps do not relaunch immediately on a stale input timestamp.
  lastReaderInteractionMs_ = millis();
  lastIdleBackgroundKickMs_ = millis();
}

bool ReaderState::ensurePageCached(Core& core, uint16_t pageNum) {
  if (!pageCache_) {
    return false;
  }

  const reader::HeapState heap = reader::readHeapState();
  if (reader::isHeapCritical(heap)) {
    pageCache_->clearResidentPages();
  } else if (reader::isHeapTight(heap)) {
    pageCache_->trimResidentPages(pageNum, 0, 2);
  }

  size_t pageCount = pageCache_->pageCount();
  const bool needsExtension = pageCache_->needsExtension(pageNum);
  const bool isPartial = pageCache_->isPartial();

  if (pageNum < pageCount) {
    if (needsExtension) {
      LOG_DBG(TAG, "Pre-extending cache at page %d", pageNum);
      if (!isWorkerRunning()) {
        startBackgroundCaching(core, "pre-extend");
      }
    }
    return true;
  }

  if (!isPartial) {
    LOG_DBG(TAG, "Page %d not available (cache complete at %d pages)", pageNum, static_cast<int>(pageCount));
    return false;
  }

  LOG_DBG(TAG, "Extending cache for page %d", pageNum);

  if (core.content.metadata().type == ContentType::Epub) {
    LOG_INF(TAG, "[ASYNC] deferring EPUB cache extend spine=%d page=%u cachedPages=%u partial=%u", currentSpineIndex_,
            static_cast<unsigned>(pageNum), static_cast<unsigned>(pageCount), static_cast<unsigned>(isPartial));
    armPendingEpubPageLoad(core, currentSpineIndex_, pageNum, false, false);
    return false;
  }

  armPendingEpubPageLoad(core, currentSpineIndex_, pageNum, false, false);
  return false;
}

void ReaderState::loadCacheFromDisk(Core& core) {
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  cacheController_.loadCacheFromDisk(core, vp);
}

void ReaderState::reloadCacheFromDisk(Core& core) {
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  cacheController_.reloadCacheFromDisk(core, vp);
}

void ReaderState::createOrExtendCache(Core& core, uint16_t batchSize) {
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  cacheController_.createOrExtendCache(core, vp, batchSize);
  invalidateAnchorMapCache();
}

void ReaderState::renderPageContents(Core& core, Page& page, int marginTop, int marginRight, int marginBottom,
                                     int marginLeft) {
  (void)marginRight;
  (void)marginBottom;

  const Theme& theme = THEME_MANAGER.current();
  const int fontId = core.settings.getReaderFontId(theme);
  TextBlock::bionicReading = core.settings.bionicReading;
  TextBlock::fakeBold = core.settings.fakeBold;
  page.render(renderer_, fontId, marginLeft, marginTop, theme.primaryTextBlack);
}

void ReaderState::renderStatusBar(Core& core, int marginRight, int marginBottom, int marginLeft, int totalPages,
                                  bool isPartial) {
  const Theme& theme = THEME_MANAGER.current();

  // Build status bar data
  ui::ReaderStatusBarData data{};
  data.mode = core.settings.statusBar;
  data.title = core.content.metadata().title;

  // Resolve chapter title if in Chapter mode (cached to avoid SD I/O on every render)
  if (data.mode == Settings::StatusChapter && core.content.tocCount() > 0) {
    if (currentSpineIndex_ != cachedChapterSpine_ || currentSectionPage_ != cachedChapterPage_) {
      cachedChapterTitle_[0] = '\0';
      int tocIndex = findCurrentTocEntry(core);
      if (tocIndex >= 0) {
        auto result = core.content.getTocEntry(tocIndex);
        if (result.ok()) {
          strncpy(cachedChapterTitle_, result.value.title, sizeof(cachedChapterTitle_) - 1);
          cachedChapterTitle_[sizeof(cachedChapterTitle_) - 1] = '\0';
        }
      }
      cachedChapterSpine_ = currentSpineIndex_;
      cachedChapterPage_ = currentSectionPage_;
    }
    if (cachedChapterTitle_[0] != '\0') {
      data.title = cachedChapterTitle_;
    }
  }

  // Battery
  const uint16_t millivolts = batteryMonitor.readMillivolts();
  data.batteryPercent = (millivolts < 100) ? -1 : BatteryMonitor::percentageFromMillivolts(millivolts);

  const GlobalPageMetrics metrics = resolveGlobalPageMetrics(core, totalPages, isPartial);
  data.currentPage = metrics.currentPage;
  data.totalPages = metrics.totalPages;
  data.isPartial = data.totalPages <= 0;

  ui::readerStatusBar(renderer_, theme, marginLeft, marginRight, marginBottom, data);
}

void ReaderState::renderXtcPage(Core& core) {
  auto* provider = core.content.asXtc();
  if (!provider) {
    return;
  }

  const Theme& theme = THEME_MANAGER.current();

  auto result = xtcRenderer_.render(provider->getParser(), currentPage_, [this, &core]() { displayWithRefresh(core); });

  switch (result) {
    case XtcPageRenderer::RenderResult::Success:
      if (provider->getParser().getBitDepth() == 2) {
        pagesUntilFullRefresh_ = 1;
      }
      break;
    case XtcPageRenderer::RenderResult::EndOfBook:
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "End of book");
      break;
    case XtcPageRenderer::RenderResult::InvalidDimensions:
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "Invalid file");
      break;
    case XtcPageRenderer::RenderResult::AllocationFailed:
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "Memory error");
      break;
    case XtcPageRenderer::RenderResult::PageLoadFailed:
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "Page load error");
      break;
  }
}

void ReaderState::displayWithRefresh(Core& core) {
  const bool turnOffScreen = core.settings.sunlightFadingFix != 0;
  const int pagesPerRefreshValue = core.settings.getPagesPerRefreshValue();

  // After an overlay banner (displayWindow), the RED RAM baseline is stale.
  // Drive-all refresh writes the inverted framebuffer to RED RAM so every
  // pixel is explicitly driven to its target state — no ghosting, no flash.
  if (forceCleanRefreshOnNext_) {
    forceCleanRefreshOnNext_ = false;
    LOG_DBG(TAG, "Refresh policy: frame=text mode=drive-all reason=overlay-cleanup");
    renderer_.displayBufferDriveAll(turnOffScreen);
    pagesUntilFullRefresh_ = pagesPerRefreshValue > 0 ? static_cast<uint8_t>(pagesPerRefreshValue) : 0;
    return;
  }

  if (core.wokeFromSleep) {
    LOG_DBG(TAG, "Refresh policy: frame=text mode=fast reason=wake");
    renderer_.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);
    core.wokeFromSleep = false;
    pagesUntilFullRefresh_ = pagesPerRefreshValue > 0 ? static_cast<uint8_t>(pagesPerRefreshValue - 1) : 0;
  } else if (pagesPerRefreshValue == 0) {
    LOG_DBG(TAG, "Refresh policy: frame=text mode=fast reason=cadence-disabled");
    renderer_.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);
    pagesUntilFullRefresh_ = 0;
  } else if (pagesUntilFullRefresh_ <= 1) {
    LOG_DBG(TAG, "Refresh policy: frame=text mode=half reason=cadence");
    renderer_.displayBuffer(EInkDisplay::HALF_REFRESH, turnOffScreen);
    pagesUntilFullRefresh_ = pagesPerRefreshValue;
  } else {
    LOG_DBG(TAG, "Refresh policy: frame=text mode=fast reason=cadence");
    renderer_.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);
    pagesUntilFullRefresh_--;
  }
}

void ReaderState::renderCenteredStatusMessage(Core& core, const char* message, int fontIdOverride) {
  const Theme& theme = THEME_MANAGER.current();
  const bool turnOffScreen = core.settings.sunlightFadingFix != 0;
  const int fontId = fontIdOverride ? fontIdOverride : core.settings.getReaderFontId(theme);

  // Calculate compact overlay banner dimensions (centered on screen).
  constexpr int padH = 24;  // horizontal padding inside banner
  constexpr int padV = 16;  // vertical padding inside banner
  const int textWidth = renderer_.getTextWidth(fontId, message, EpdFontFamily::BOLD);
  const int lineHeight = renderer_.getLineHeight(fontId);
  const int bannerW = textWidth + padH * 2;
  const int bannerH = lineHeight + padV * 2;
  const int bannerX = (renderer_.getScreenWidth() - bannerW) / 2;
  const int bannerY = (renderer_.getScreenHeight() - bannerH) / 2;

  // Draw overlay banner on top of existing framebuffer content — no clearScreen,
  // so the previous page / file-list stays visible behind the banner.
  renderer_.fillRect(bannerX, bannerY, bannerW, bannerH, !theme.primaryTextBlack);
  renderer_.drawText(fontId, bannerX + padH, bannerY + padV, message, theme.primaryTextBlack, EpdFontFamily::BOLD);
  renderer_.drawRect(bannerX + 3, bannerY + 3, bannerW - 6, bannerH - 6, theme.primaryTextBlack);

  // Use drive-all refresh for the full screen so every pixel — including the
  // file-list / page behind the banner — is actively driven to its correct
  // state.  A simple displayWindow (partial BW write + full-panel fast scan)
  // causes cross-refresh ghosting: the scan disturbs e-ink particles that were
  // recently driven by the previous state's fast refresh but haven't fully
  // settled, making the prior frame's cursor / content visibly reappear.
  // Drive-all eliminates this by making every pixel appear "changed" (inverted
  // RED RAM), so the SSD1677 applies a full driving pulse everywhere.
  LOG_DBG(TAG, "Refresh policy: frame=overlay mode=drive-all reason=banner (%dx%d at %d,%d)",
          bannerW, bannerH, bannerX, bannerY);
  renderer_.displayBufferDriveAll(turnOffScreen);
  core.display.markDirty();

  // Drive-all already established a clean differential baseline, but the next
  // page render will replace the entire screen (banner + background → book
  // content).  Keep the flag so that transition also uses drive-all for a
  // crisp first page without ghosting from the banner.
  forceCleanRefreshOnNext_ = true;
}

ReaderState::Viewport ReaderState::getReaderViewport(bool showStatusBar) const {
  Viewport vp{};
  renderer_.getOrientedViewableTRBL(&vp.marginTop, &vp.marginRight, &vp.marginBottom, &vp.marginLeft);
  vp.marginLeft += horizontalPadding;
  vp.marginRight += horizontalPadding;
  if (showStatusBar) {
    vp.marginBottom += statusBarMargin;
  }
  vp.width = renderer_.getScreenWidth() - vp.marginLeft - vp.marginRight;
  vp.height = renderer_.getScreenHeight() - vp.marginTop - vp.marginBottom;
  return vp;
}

bool ReaderState::renderCoverPage(Core& core) {
  LOG_DBG(TAG, "Generating cover for reader...");
  if (core.content.metadata().type == ContentType::Epub) {
    const std::string existingCoverPath = core.content.getCoverPath();
    const bool coverCached = !existingCoverPath.empty() && SdMan.exists(existingCoverPath.c_str());
    if (!coverCached) {
      const reader::HeapState heap = reader::readHeapState();
      if (reader::isHeapCritical(heap)) {
        LOG_INF(TAG, "Skipping uncached EPUB cover due to tight heap free=%u largest=%u",
                static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlock));
        return false;
      }
    }
  }

  std::string coverPath = core.content.generateCover(true);  // Always 1-bit in reader (saves ~48KB grayscale buffer)
  if (coverPath.empty()) {
    LOG_DBG(TAG, "No cover available, skipping cover page");
    return false;
  }

  LOG_DBG(TAG, "Rendering cover page from: %s", coverPath.c_str());
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  int pagesUntilRefresh = pagesUntilFullRefresh_;
  const bool turnOffScreen = core.settings.sunlightFadingFix != 0;

  bool rendered = CoverHelpers::renderCoverFromBmp(renderer_, coverPath, vp.marginTop, vp.marginRight, vp.marginBottom,
                                                   vp.marginLeft, pagesUntilRefresh,
                                                   core.settings.getPagesPerRefreshValue(), turnOffScreen);

  // После полноэкранной обложки следующая текстовая страница должна пройти через
  // обычную более "тяжёлую" ступень cadence, но без отдельного промежуточного кадра.
  pagesUntilFullRefresh_ = 1;
  return rendered;
}

bool ReaderState::isWorkerRunning() const { return asyncJobs_.isJobRunning(); }

BackgroundTask::State ReaderState::workerState() const { return asyncJobs_.workerState(); }

void ReaderState::requestWorkerCancel() { asyncJobs_.requestCancelCurrentJob(); }

bool ReaderState::waitWorkerIdle(const uint32_t maxWaitMs) { return asyncJobs_.waitUntilIdle(maxWaitMs); }

void ReaderState::startBackgroundCaching(Core& core, const char* trigger) {
  if (!contentLoaded_) {
    return;
  }

  // Refuse all background cache work when heap is critically low.
  // Cold extends allocate a full ContentParser; if that pushes free heap
  // below ~10 KB the SdFat driver's internal state can become corrupt,
  // causing cascading SD-card access failures (files vanishing, directories
  // unreadable) that persist until reboot.
  {
    const auto heap = reader::readHeapState();
    if (reader::isHeapCritical(heap)) {
      LOG_DBG(TAG, "[ASYNC] skip background cache: heap critical (free=%u largest=%u)",
              static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlock));
      return;
    }
  }

  if (pendingTocJumpActive_ || pendingEpubPageLoadActive_ || tocMode_ || bookmarkMode_ || menuMode_) {
    LOG_INF(TAG,
            "[ASYNC] skip background cache during pending/overlay state (toc=%u bookmark=%u menu=%u tocJump=%u pageLoad=%u)",
            static_cast<unsigned>(tocMode_), static_cast<unsigned>(bookmarkMode_), static_cast<unsigned>(menuMode_),
            static_cast<unsigned>(pendingTocJumpActive_), static_cast<unsigned>(pendingEpubPageLoadActive_));
    return;
  }

  const BackgroundCachePlan plan = planBackgroundCacheWork(core);
  if (!plan.shouldStart) {
    return;
  }

  if (trigger && strcmp(trigger, "post-render") == 0 && plan.reason == BackgroundCacheWakeReason::FarPrefetchReady) {
    LOG_DBG(TAG, "[CACHE] background cache skip trigger=%s reason=far-prefetch-deferred activeSpine=%d candidate=%d",
            trigger, plan.activeSpine, plan.candidateSpine);
    return;
  }

  if (core.content.metadata().type == ContentType::Xtc) {
    if (!thumbnailDone_) {
      core.content.generateCover(true);
      core.content.generateThumbnail();
      thumbnailDone_ = true;
    }
    return;
  }

  if (isWorkerRunning()) {
    LOG_DBG(TAG, "[ASYNC] background cache request ignored because worker is already busy");
    return;
  }

  reader::ReaderAsyncJobsController::BackgroundCacheRequest request;
  request.position.currentPage = currentPage_;
  request.position.currentSpineIndex = currentSpineIndex_;
  request.position.currentSectionPage = currentSectionPage_;
  request.position.lastRenderedSpineIndex = lastRenderedSpineIndex_;
  request.position.lastRenderedSectionPage = lastRenderedSectionPage_;
  request.position.hasCover = hasCover_;
  request.position.textStartIndex = textStartIndex_;
  request.plan = plan;
  request.showStatusBar = core.settings.statusBar != 0;
  if (trigger) {
    strlcpy(request.trigger, trigger, sizeof(request.trigger));
  }

  lastIdleBackgroundKickMs_ = millis();
  if (!asyncJobs_.queueBackgroundCache(request)) {
    LOG_ERR(TAG, "[ASYNC] failed to queue background cache trigger=%s reason=%s", trigger ? trigger : "auto",
            backgroundCacheWakeReasonToString(plan.reason));
  }
}

bool ReaderState::stopBackgroundCaching() {
  if (!isWorkerRunning()) {
    return true;
  }

  requestWorkerCancel();
  if (!waitWorkerIdle(kCacheTaskStopTimeoutMs)) {
    LOG_ERR(TAG, "[ASYNC] worker did not stop within timeout");
    LOG_ERR(TAG, "Restarting to avoid unsafe cache/parser teardown after stop timeout");
    vTaskDelay(50 / portTICK_PERIOD_MS);
    ESP.restart();
    return false;
  }
  return true;
}

}  // namespace papyrix
