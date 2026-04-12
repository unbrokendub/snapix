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
#include <esp_timer.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
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
#include "reader/ReaderSupport.h"

#define TAG "READER"

namespace papyrix {
using reader::kCacheTaskStackSize;
using reader::kCacheTaskStopTimeoutMs;
using reader::kDefaultCacheBatchPages;
using reader::kIdleBackgroundKickIntervalMs;
using reader::kInteractiveCacheTaskPriority;
using reader::kNonResumableCacheBatchPages;
using reader::kPendingEpubPageLoadMaxRetries;
using reader::kPendingTocJumpMaxRetries;

namespace {
#ifndef PAPYRIX_PERF_LOG
#define PAPYRIX_PERF_LOG 0
#endif

constexpr int horizontalPadding = 5;
constexpr int statusBarMargin = 23;
constexpr int kFb2TocJumpHeadroomPages = 6;
constexpr int kFb2TocJumpMinimumGrowthPages = 24;
constexpr int kFb2TocJumpColdStartCapPages = 72;
constexpr int kFb2TocHintInflationDivisor = 2;
constexpr size_t kEstimatedBytesPerPage = 2048;

uint16_t estimatePagesForBytes(const size_t bytes, const size_t bytesPerPage = kEstimatedBytesPerPage) {
  const size_t safeBytesPerPage = std::max<size_t>(1, bytesPerPage);
  const size_t pageCount = std::max<size_t>(1, (bytes + safeBytesPerPage - 1) / safeBytesPerPage);
  return static_cast<uint16_t>(std::min<size_t>(pageCount, UINT16_MAX));
}

bool fb2UsesSectionNavigation(const Fb2Provider* provider) {
  return provider && provider->getFb2() && provider->tocCount() > 0;
}

bool resolveFb2SectionContext(const Fb2Provider* provider, const RenderConfig& config, const int tocIndex,
                              std::string* cachePath, uint32_t* startOffset, int* startingSectionIndex) {
  if (!fb2UsesSectionNavigation(provider) || tocIndex < 0 || tocIndex >= static_cast<int>(provider->tocCount())) {
    return false;
  }

  const Fb2::TocItem item = provider->getFb2()->getTocItem(static_cast<uint16_t>(tocIndex));
  if (item.sectionIndex < 0) {
    return false;
  }

  if (cachePath) {
    *cachePath = reader::fb2SectionCachePath(provider->getFb2()->getCachePath(), config.fontId, tocIndex);
  }
  if (startOffset) {
    *startOffset = item.sourceOffset;
  }
  if (startingSectionIndex) {
    *startingSectionIndex = item.sectionIndex;
  }
  return true;
}

int estimateFb2TocTargetPageHint(const Fb2* fb2, const uint32_t estimatedTotalPages, const Fb2::TocItem& tocItem) {
  if (!fb2 || estimatedTotalPages == 0) {
    return -1;
  }

  const size_t fileSize = fb2->getFileSize();
  if (fileSize == 0) {
    return -1;
  }

  if (tocItem.sourceOffset > 0 && tocItem.sourceOffset < fileSize) {
    const uint64_t scaled = static_cast<uint64_t>(tocItem.sourceOffset) * estimatedTotalPages;
    const int baseHint = static_cast<int>(scaled / fileSize);
    return baseHint + std::max(8, baseHint / kFb2TocHintInflationDivisor);
  }

  return tocItem.sectionIndex >= 0 ? tocItem.sectionIndex : -1;
}

uint32_t resolveFb2AnchorSourceOffset(const Fb2* fb2, const std::string& anchor) {
  if (!fb2) {
    return 0;
  }

  constexpr char kPrefix[] = "section_";
  if (anchor.rfind(kPrefix, 0) != 0) {
    return 0;
  }

  const int targetSectionIndex = atoi(anchor.c_str() + static_cast<int>(sizeof(kPrefix) - 1));
  if (targetSectionIndex < 0) {
    return 0;
  }

  const uint16_t tocCount = fb2->tocCount();
  for (uint16_t i = 0; i < tocCount; ++i) {
    const Fb2::TocItem item = fb2->getTocItem(i);
    if (item.sectionIndex == targetSectionIndex) {
      return item.sourceOffset;
    }
  }

  return 0;
}

inline uint32_t perfMsNow() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

inline void perfLog(const char* phase, uint32_t startedMs, const char* fmt = nullptr, ...) {
#if PAPYRIX_PERF_LOG
  char suffix[128] = "";
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

// Cache path helpers
inline std::string epubSectionCachePath(const std::string& epubCachePath, int spineIndex) {
  return epubCachePath + "/sections/" + std::to_string(spineIndex) + ".bin";
}

inline std::string contentCachePath(const char* cacheDir, int fontId) {
  return std::string(cacheDir) + "/pages_" + std::to_string(fontId) + ".bin";
}

struct ReaderHeapState {
  size_t freeBytes = 0;
  size_t largestBlock = 0;
};

inline ReaderHeapState readReaderHeapState() {
  return ReaderHeapState{heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)};
}

inline bool isReaderHeapCritical(const ReaderHeapState& heap) {
  return heap.freeBytes < 72 * 1024 || heap.largestBlock < 36 * 1024;
}

inline bool isReaderHeapTight(const ReaderHeapState& heap) {
  return heap.freeBytes < 96 * 1024 || heap.largestBlock < 52 * 1024;
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
    LOG_ERR(TAG, "[ASYNC] arm page-load spine=%d page=%d complete=%u indexing=%u", targetSpine, targetPage,
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

  if (core.content.metadata().type != ContentType::Epub) {
    return;
  }

  auto* provider = core.content.asEpub();
  if (!provider || !provider->getEpub()) {
    return;
  }

  auto epub = provider->getEpubShared();
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) {
    return;
  }

  globalSectionPageMetrics_.resize(static_cast<size_t>(spineCount));
  std::vector<size_t> itemSizes(static_cast<size_t>(spineCount), 0);

  const Theme& theme = THEME_MANAGER.current();
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  const auto config = core.settings.getRenderConfig(theme, vp.width, vp.height);

  size_t calibrationBytes = 0;
  uint32_t calibrationPages = 0;

  for (int spineIndex = 0; spineIndex < spineCount; ++spineIndex) {
    const auto spineItem = epub->getSpineItem(spineIndex);
    size_t itemSize = 0;
    if (epub->getItemSize(spineItem.href, &itemSize)) {
      itemSizes[static_cast<size_t>(spineIndex)] = itemSize;
    }

    uint16_t pages = 0;
    bool exact = false;

    const std::string cachePath = epubSectionCachePath(epub->getCachePath(), spineIndex);
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
  if (core.content.metadata().type != ContentType::Epub) {
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
  const ReaderHeapState heapBeforeTrim = readReaderHeapState();
  const size_t fontBytesBeforeTrim = FONT_MANAGER.getTotalFontMemoryUsage();
  THEME_MANAGER.clearCache();
  FONT_MANAGER.unloadReaderFonts();
  renderer_.clearWidthCache();
  const ReaderHeapState heapAfterTrim = readReaderHeapState();
  LOG_INF(TAG, "Entry heap trim: free=%u->%u largest=%u->%u fontBytes=%u",
          static_cast<unsigned>(heapBeforeTrim.freeBytes), static_cast<unsigned>(heapAfterTrim.freeBytes),
          static_cast<unsigned>(heapBeforeTrim.largestBlock), static_cast<unsigned>(heapAfterTrim.largestBlock),
          static_cast<unsigned>(fontBytesBeforeTrim));

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
      LOG_ERR(TAG, "[INPUT] executing deferred page-turn dir=%d wait=%lu remaining=%d", queuedTurn,
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
               !bookmarkMode_ && !tocMode_ && !isWorkerRunning()) {
      LOG_ERR(TAG, "[CACHE] refreshing current page after background cache rewrite spine=%d page=%d",
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
              LOG_ERR(TAG, "[INPUT] preempt requested button=%d workerState=%d", static_cast<int>(e.button),
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
                LOG_ERR(TAG, "[INPUT] preempt requested button=%d workerState=%d", static_cast<int>(e.button),
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
              LOG_ERR(TAG, "[INPUT] page-turn release button=%d workerActive=%u", static_cast<int>(e.button),
                      static_cast<unsigned>(isWorkerRunning()));
              navigateNext(core);
              break;
            case Button::Left:
            case Button::Up:
              LOG_ERR(TAG, "[INPUT] page-turn release button=%d workerActive=%u", static_cast<int>(e.button),
                      static_cast<unsigned>(isWorkerRunning()));
              navigatePrev(core);
              break;
            case Button::Power:
              if (core.settings.shortPwrBtn == Settings::PowerPageTurn && powerPressStartedMs_ != 0) {
                const uint32_t heldMs = millis() - powerPressStartedMs_;
                if (heldMs < core.settings.getPowerButtonDuration()) {
                  LOG_ERR(TAG, "[INPUT] page-turn release button=%d workerActive=%u", static_cast<int>(e.button),
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
    LOG_ERR(TAG, "[NAV] next recovering from empty active cache spine=%d targetPage=%d", currentSpineIndex_,
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
    LOG_ERR(TAG, "[NAV] next no-op type=%d spine=%d page=%d cache=%u pages=%u partial=%u", static_cast<int>(type),
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
    LOG_ERR(TAG, "[NAV] prev recovering from empty active cache spine=%d targetPage=%d", currentSpineIndex_,
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
    LOG_ERR(TAG, "[NAV] prev no-op type=%d spine=%d page=%d cache=%u pages=%u partial=%u", static_cast<int>(type),
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
  needsRender_ = result.needsRender;
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
  const uint32_t totalStartMs = perfMsNow();
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
      LOG_ERR(TAG, "[ASYNC] forcing worker stop before detached warm render state=%d", static_cast<int>(workerState()));
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
    const uint32_t cacheBootstrapMs = perfMsNow();
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
      if (type == ContentType::Epub) {
        LOG_ERR(TAG, "[ASYNC] deferring foreground EPUB cache build spine=%d page=%d complete=%u", currentSpineIndex_,
                targetPage, static_cast<unsigned>(requireComplete));
        armPendingEpubPageLoad(core, currentSpineIndex_, targetPage, requireComplete, true);
        return;
      } else {
        armPendingEpubPageLoad(core, currentSpineIndex_, targetPage, requireComplete, true);
        return;
      }
    }
    perfLog("reader-cache-bootstrap", cacheBootstrapMs, "(spine=%d page=%d)", currentSpineIndex_, currentSectionPage_);

    // Clamp page number (handle negative values and out-of-bounds)
    if (pageCache_) {
      const int cachedPages = static_cast<int>(pageCache_->pageCount());
      if (currentSectionPage_ < 0) {
        currentSectionPage_ = 0;
      } else if (currentSectionPage_ >= cachedPages) {
        currentSectionPage_ = cachedPages > 0 ? cachedPages - 1 : 0;
      }
    }
  }

  // Check if we need to extend cache
  if (!ensurePageCached(core, currentSectionPage_)) {
    if (type == ContentType::Epub && pendingEpubPageLoadActive_) {
      return;
    }
    renderer_.drawCenteredText(core.settings.getReaderFontId(theme), 300, "Failed to load page", theme.primaryTextBlack,
                               BOLD);
    renderer_.displayBuffer();
    needsRender_ = false;  // Prevent infinite render loop on cache failure
    return;
  }

  // Load and render page (cache is now guaranteed to exist, we own it)
  size_t pageCount = pageCache_ ? pageCache_->pageCount() : 0;
  const uint32_t pageLoadMs = perfMsNow();
  std::shared_ptr<Page> page = pageCache_ ? pageCache_->loadPage(currentSectionPage_) : nullptr;
  perfLog("reader-page-load", pageLoadMs, "(spine=%d page=%d)", currentSpineIndex_, currentSectionPage_);

  if (!page) {
    LOG_ERR(TAG, "Failed to load page, clearing cache");
    if (pageCache_) {
      pageCache_->clear();
      pageCache_.reset();
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

  const uint32_t renderBwMs = perfMsNow();
  renderPageContents(core, *page, vp.marginTop, vp.marginRight, vp.marginBottom, vp.marginLeft);
  renderStatusBar(core, vp.marginRight, vp.marginBottom, vp.marginLeft, static_cast<int>(pageCount), cacheIsPartial);
  perfLog("reader-render-bw", renderBwMs, "(aa=%u images=%u)", static_cast<unsigned>(aaEnabled),
          static_cast<unsigned>(page->hasImages()));

  const uint32_t displayMs = perfMsNow();
  displayWithRefresh(core);
  perfLog("reader-display-main", displayMs, nullptr);

  // Grayscale text rendering (anti-aliasing)
  if (aaEnabled) {
    const uint32_t aaMs = perfMsNow();
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
    perfLog("reader-aa-pass", aaMs, nullptr);
  }

  const uint32_t prefetchMs = perfMsNow();
  if (allowPagePrefetch) {
    prefetchAdjacentPage(core);
    perfLog("reader-prefetch", prefetchMs, nullptr);
  } else {
    perfLog("reader-prefetch-skip", prefetchMs, "(reason=detached-fast-turn)");
  }

  LOG_DBG(TAG, "Rendered page %d/%d", currentSectionPage_ + 1, pageCount);
  perfLog("reader-total", totalStartMs, "(spine=%d page=%d)", currentSpineIndex_, currentSectionPage_);
  // Count actual visible page presentation as reader activity so far-idle
  // sweeps do not relaunch immediately on a stale input timestamp.
  lastReaderInteractionMs_ = millis();
  lastIdleBackgroundKickMs_ = millis();
}

bool ReaderState::ensurePageCached(Core& core, uint16_t pageNum) {
  if (!pageCache_) {
    return false;
  }

  const ReaderHeapState heap = readReaderHeapState();
  if (isReaderHeapCritical(heap)) {
    pageCache_->clearResidentPages();
  } else if (isReaderHeapTight(heap)) {
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
    LOG_ERR(TAG, "[ASYNC] deferring EPUB cache extend spine=%d page=%u cachedPages=%u partial=%u", currentSpineIndex_,
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

void ReaderState::renderCenteredStatusMessage(Core& core, const char* message) {
  const Theme& theme = THEME_MANAGER.current();
  const bool turnOffScreen = core.settings.sunlightFadingFix != 0;
  const int fontId = core.settings.getReaderFontId(theme);
  const int y = renderer_.getScreenHeight() / 2 - renderer_.getLineHeight(fontId) / 2;

  renderer_.clearScreen(theme.backgroundColor);
  renderer_.drawCenteredText(fontId, y, message, theme.primaryTextBlack, EpdFontFamily::BOLD);
  LOG_DBG(TAG, "Refresh policy: frame=overlay mode=fast reason=transient");
  renderer_.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);
  core.display.markDirty();
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
      const ReaderHeapState heap = readReaderHeapState();
      if (isReaderHeapCritical(heap)) {
        LOG_ERR(TAG, "Skipping uncached EPUB cover due to tight heap free=%u largest=%u",
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

  if (pendingTocJumpActive_ || pendingEpubPageLoadActive_ || tocMode_ || bookmarkMode_ || menuMode_) {
    LOG_ERR(TAG,
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

void ReaderState::runBackgroundCacheJob(const reader::ReaderAsyncJobsController::BackgroundCacheRequest& request,
                                        const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort) {
  Core* corePtr = activeCore_;
  if (!corePtr) {
    LOG_ERR(TAG, "[ASYNC] background cache worker aborted: no active core");
    return;
  }

  Core& coreRef = *corePtr;
  const Theme& theme = THEME_MANAGER.current();
  const auto vp = getReaderViewport(request.showStatusBar);
  const auto config = coreRef.settings.getRenderConfig(theme, vp.width, vp.height);

  LOG_INF(TAG, "Background cache task started");
  LOG_ERR(TAG, "[CACHE] worker state trigger=%s reason=%s candidate=%d farSweep=%u state=running", request.trigger,
          backgroundCacheWakeReasonToString(request.plan.reason), request.plan.candidateSpine,
          static_cast<unsigned>(request.plan.allowFarSweep));

  bool workerDidBackgroundWork = false;
  const bool acquired = cacheController_.withWorkerResources("background-cache", [&](auto&) {
    ContentType type = coreRef.content.metadata().type;
    std::string cachePath;
    int activeSpineForCache = request.position.currentSpineIndex;

    if (type == ContentType::Epub) {
      auto* provider = coreRef.content.asEpub();
      if (provider && provider->getEpub() && !(shouldAbort && shouldAbort())) {
        const auto* epub = provider->getEpub();
        const std::string imageCachePath = coreRef.settings.showImages ? (epub->getCachePath() + "/images") : "";
        if (request.position.currentSectionPage == -1) {
          activeSpineForCache =
              calcFirstContentSpine(request.position.hasCover, request.position.textStartIndex, epub->getSpineItemsCount());
        }
        cachePath = epubSectionCachePath(epub->getCachePath(), activeSpineForCache);

        if (!parser_ || parserSpineIndex_ != activeSpineForCache) {
          if (!promoteLookaheadParser(activeSpineForCache)) {
            parser_.reset(new EpubChapterParser(provider->getEpubShared(), activeSpineForCache, renderer_, config,
                                               imageCachePath, true));
            parserSpineIndex_ = activeSpineForCache;
          }
        }
      }
    } else if (type == ContentType::Markdown && !(shouldAbort && shouldAbort())) {
      cachePath = contentCachePath(coreRef.content.cacheDir(), config.fontId);
      if (!parser_) {
        parser_.reset(new MarkdownParser(contentPath_, renderer_, config));
        parserSpineIndex_ = 0;
      }
    } else if (type == ContentType::Fb2 && !(shouldAbort && shouldAbort())) {
      auto* fb2Provider = coreRef.content.asFb2();
      uint32_t startOffset = 0;
      int startingSectionIndex = 0;
      if (resolveFb2SectionContext(fb2Provider, config, activeSpineForCache, &cachePath, &startOffset,
                                   &startingSectionIndex)) {
        if (!parser_ || parserSpineIndex_ != activeSpineForCache) {
          parser_.reset(new Fb2Parser(contentPath_, renderer_, config, startOffset, startingSectionIndex, true));
          parserSpineIndex_ = activeSpineForCache;
        }
      } else {
        cachePath = contentCachePath(coreRef.content.cacheDir(), config.fontId);
        if (!parser_) {
          parser_.reset(new Fb2Parser(contentPath_, renderer_, config));
          parserSpineIndex_ = 0;
        }
      }
    } else if (type == ContentType::Html && !(shouldAbort && shouldAbort())) {
      cachePath = contentCachePath(coreRef.content.cacheDir(), config.fontId);
      if (!parser_) {
        parser_.reset(new HtmlParser(contentPath_, coreRef.content.cacheDir(), renderer_, config));
        parserSpineIndex_ = 0;
      }
    } else if (type == ContentType::Txt && !(shouldAbort && shouldAbort())) {
      cachePath = contentCachePath(coreRef.content.cacheDir(), config.fontId);
      if (!parser_) {
        parser_.reset(new PlainTextParser(contentPath_, renderer_, config));
        parserSpineIndex_ = 0;
      }
    }

    if (!parser_ || cachePath.empty() || (shouldAbort && shouldAbort())) {
      return;
    }

    const int safeSectionPage = request.position.currentSectionPage < 0 ? 0 : request.position.currentSectionPage;
    const bool sameCache = pageCache_ && pageCache_->path() == cachePath;
    bool shouldRefreshCurrentPageAfterUpdate = false;
    bool didBackgroundWork = false;

    if (!pageCache_) {
      const size_t pagesBefore = 0;
      cacheController_.backgroundCacheImpl(*parser_, cachePath, config, safeSectionPage, shouldAbort);
      didBackgroundWork =
          pageCache_ && pageCache_->path() == cachePath && (pageCache_->pageCount() > pagesBefore || !pageCache_->isPartial());
      shouldRefreshCurrentPageAfterUpdate =
          didBackgroundWork &&
          (type == ContentType::Epub && request.position.currentSectionPage >= 0 &&
           request.position.currentSpineIndex == activeSpineForCache);
    } else if (sameCache && pageCache_->isPartial()) {
      const bool canHotExtend = parser_->canResume();
      const bool nearTail = pageCache_->needsExtension(static_cast<uint16_t>(safeSectionPage));
      const bool eagerIdleHotExtend = (type == ContentType::Epub && canHotExtend);

      if (nearTail || eagerIdleHotExtend) {
        const bool mayRewriteExistingPages = !canHotExtend;
        const size_t pagesBefore = pageCache_->pageCount();
        const bool partialBefore = pageCache_->isPartial();
        cacheController_.backgroundCacheImpl(*parser_, cachePath, config, safeSectionPage, shouldAbort,
                                             eagerIdleHotExtend);
        didBackgroundWork = pageCache_ && pageCache_->path() == cachePath &&
                            (pageCache_->pageCount() != pagesBefore || pageCache_->isPartial() != partialBefore);
        shouldRefreshCurrentPageAfterUpdate =
            didBackgroundWork && (type == ContentType::Epub && mayRewriteExistingPages &&
                                  request.position.currentSectionPage >= 0 &&
                                  request.position.currentSpineIndex == activeSpineForCache);
        if (shouldRefreshCurrentPageAfterUpdate) {
          warmedNextPage_.clear();
          warmedNextNextPage_.clear();
          renderOverridePage_.clear();
        }
      }
    }

    if (shouldRefreshCurrentPageAfterUpdate && pageCache_ && pageCache_->path() == cachePath &&
        request.position.currentSectionPage < static_cast<int>(pageCache_->pageCount())) {
      pendingBackgroundEpubRefresh_ = true;
      pendingBackgroundEpubRefreshSpine_ = request.position.currentSpineIndex;
      pendingBackgroundEpubRefreshPage_ = request.position.currentSectionPage;
      LOG_ERR(TAG, "[CACHE] scheduled repaint after background cache update spine=%d page=%d",
              request.position.currentSpineIndex, request.position.currentSectionPage);
    }

    if (type == ContentType::Epub && !(shouldAbort && shouldAbort())) {
      const bool currentCacheComplete = pageCache_ && pageCache_->path() == cachePath && !pageCache_->isPartial();
      if (currentCacheComplete) {
        didBackgroundWork =
            cacheController_.prefetchNextEpubSpineCache(coreRef, config, activeSpineForCache, request.position.hasCover,
                                                        request.position.textStartIndex, request.plan.allowFarSweep,
                                                        shouldAbort) ||
            didBackgroundWork;
      } else {
        cacheController_.resetBackgroundPrefetchState();
      }
    }

    workerDidBackgroundWork = didBackgroundWork;
  });

  if (!acquired) {
    LOG_ERR(TAG, "[OWNERSHIP] background cache worker could not acquire document resources");
    return;
  }

  LOG_ERR(TAG, "[CACHE] worker state trigger=%s reason=%s candidate=%d farSweep=%u state=%s didWork=%u stopRequested=%u",
          request.trigger, backgroundCacheWakeReasonToString(request.plan.reason), request.plan.candidateSpine,
          static_cast<unsigned>(request.plan.allowFarSweep), (shouldAbort && shouldAbort()) ? "stopping" : "complete",
          static_cast<unsigned>(workerDidBackgroundWork), static_cast<unsigned>((shouldAbort && shouldAbort()) ? 1 : 0));
}

void ReaderState::runTocJumpJob(const reader::ReaderAsyncJobsController::TocJumpRequest& request,
                                const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort) {
  Core* corePtr = activeCore_;
  if (!corePtr) {
    LOG_ERR(TAG, "[ASYNC] TOC worker aborted: no active core");
    return;
  }

  Core& coreRef = *corePtr;
  const ContentType type = coreRef.content.metadata().type;
  if (type != ContentType::Epub && type != ContentType::Fb2) {
    return;
  }

  if (type == ContentType::Fb2 && fb2UsesSectionNavigation(coreRef.content.asFb2())) {
    return;
  }

  const Theme& theme = THEME_MANAGER.current();
  const auto vp = getReaderViewport(coreRef.settings.statusBar != 0);
  const auto config = coreRef.settings.getRenderConfig(theme, vp.width, vp.height);
  const std::string targetAnchor = request.anchor;

  LOG_ERR(TAG, "[ASYNC] TOC worker entered spine=%d anchor=%s", request.targetSpine, targetAnchor.c_str());

  if (type == ContentType::Epub) {
    auto* provider = coreRef.content.asEpub();
    if (!provider || !provider->getEpub()) {
      return;
    }

    auto epub = provider->getEpubShared();
    const std::string cachePath = epubSectionCachePath(epub->getCachePath(), request.targetSpine);
    const std::string imageCachePath = coreRef.settings.showImages ? (epub->getCachePath() + "/images") : "";

    EpubChapterParser parser(epub, request.targetSpine, renderer_, config, imageCachePath, true);
    PageCache cache(cachePath);
    bool cacheLoaded = cache.load(config);
    if (cacheLoaded && !SdMan.exists((cachePath + ".anchors").c_str())) {
      cacheLoaded = false;
      cache.clear();
    }

    auto anchorResolved = [&]() -> bool {
      if (targetAnchor.empty()) {
        return cache.pageCount() > 0;
      }
      return loadAnchorPage(cachePath, targetAnchor) >= 0;
    };

    if (anchorResolved()) {
      return;
    }

    while (!(shouldAbort && shouldAbort())) {
      const bool success = cacheLoaded ? cache.extend(parser, PageCache::DEFAULT_CACHE_CHUNK, shouldAbort)
                                       : cache.create(parser, config, PageCache::DEFAULT_CACHE_CHUNK, 0, shouldAbort);
      if (!success) {
        LOG_ERR(TAG, "[ASYNC] TOC worker failed spine=%d", request.targetSpine);
        break;
      }

      saveAnchorMap(parser, cachePath);
      if (anchorResolved() || !cache.isPartial()) {
        break;
      }

      cacheLoaded = true;
      vTaskDelay(1 / portTICK_PERIOD_MS);
    }
    return;
  }

  const std::string cachePath = contentCachePath(coreRef.content.cacheDir(), config.fontId);
  auto* provider = coreRef.content.asFb2();
  auto* fb2 = provider ? provider->getFb2() : nullptr;
  const uint32_t targetSourceOffset = resolveFb2AnchorSourceOffset(fb2, targetAnchor);
  Fb2Parser parser(contentPath_, renderer_, config);
  PageCache cache(cachePath);
  bool cacheLoaded = cache.load(config);
  int targetPageHint = request.targetPageHint;
  if (cacheLoaded && !SdMan.exists((cachePath + ".anchors").c_str())) {
    cacheLoaded = false;
    cache.clear();
  }

  auto anchorResolved = [&]() -> bool {
    if (targetAnchor.empty()) {
      return cache.pageCount() > 0;
    }
    return loadAnchorPage(cachePath, targetAnchor) >= 0;
  };

  if (anchorResolved()) {
    return;
  }

  while (!(shouldAbort && shouldAbort())) {
    const bool resumable = parser.canResume();
    const int currentPages = static_cast<int>(cache.pageCount());
    const int growthPages = std::max(kFb2TocJumpMinimumGrowthPages, currentPages / 3);
    int desiredTotalPages = currentPages > 0 ? currentPages + growthPages : kFb2TocJumpMinimumGrowthPages;
    if (targetPageHint >= 0) {
      desiredTotalPages = std::max(desiredTotalPages, targetPageHint + kFb2TocJumpHeadroomPages);
    }
    desiredTotalPages = std::max(desiredTotalPages, currentPages + 1);
    if (!resumable && targetPageHint >= 0 && desiredTotalPages > currentPages) {
      const int cappedGrowth =
          std::max(kFb2TocJumpMinimumGrowthPages, std::min(kFb2TocJumpColdStartCapPages, desiredTotalPages - currentPages));
      desiredTotalPages = std::min(desiredTotalPages, currentPages + cappedGrowth);
    }

    const uint16_t batchSize =
        cacheLoaded ? static_cast<uint16_t>(std::max(1, desiredTotalPages - currentPages))
                    : static_cast<uint16_t>(desiredTotalPages);

    LOG_ERR(TAG, "[ASYNC] FB2 TOC build anchor=%s current=%u target=%u hint=%d resumable=%u", targetAnchor.c_str(),
            static_cast<unsigned>(cache.pageCount()), static_cast<unsigned>(desiredTotalPages),
            targetPageHint, static_cast<unsigned>(resumable));

    const bool success =
        cacheLoaded ? cache.extend(parser, batchSize, shouldAbort) : cache.create(parser, config, batchSize, 0, shouldAbort);
    if (!success) {
      LOG_ERR(TAG, "[ASYNC] FB2 TOC worker failed anchor=%s hint=%d", targetAnchor.c_str(), targetPageHint);
      break;
    }

    saveAnchorMap(parser, cachePath);
    if (anchorResolved() || !cache.isPartial()) {
      break;
    }

    const uint32_t parsedOffset = parser.lastParsedOffset();
    if (targetSourceOffset > 0 && parsedOffset > 0 && parsedOffset < targetSourceOffset && cache.pageCount() > 0) {
      const uint64_t scaled = static_cast<uint64_t>(cache.pageCount()) * targetSourceOffset;
      const int refinedHint = static_cast<int>(scaled / parsedOffset);
      if (refinedHint > targetPageHint) {
        targetPageHint = refinedHint + std::max(4, refinedHint / 8);
        LOG_ERR(TAG,
                "[ASYNC] FB2 TOC refine anchor=%s parsedOffset=%u targetOffset=%u refinedHint=%d",
                targetAnchor.c_str(), static_cast<unsigned>(parsedOffset), static_cast<unsigned>(targetSourceOffset),
                targetPageHint);
      }
    }

    cacheLoaded = true;
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

void ReaderState::runPageFillJob(const reader::ReaderAsyncJobsController::PageFillRequest& request,
                                 const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort) {
  Core* corePtr = activeCore_;
  if (!corePtr) {
    LOG_ERR(TAG, "[ASYNC] PageFill worker aborted: no active core");
    return;
  }

  Core& coreRef = *corePtr;
  const ContentType type = coreRef.content.metadata().type;
  const bool isEpub = type == ContentType::Epub;
  const bool isFlatPaged = type == ContentType::Fb2 || type == ContentType::Markdown || type == ContentType::Txt ||
                           type == ContentType::Html;
  if (!isEpub && !isFlatPaged) {
    return;
  }

  auto* provider = coreRef.content.asEpub();
  if (isEpub && (!provider || !provider->getEpub())) {
    return;
  }

  const Theme& theme = THEME_MANAGER.current();
  const auto vp = getReaderViewport(coreRef.settings.statusBar != 0);
  const auto config = coreRef.settings.getRenderConfig(theme, vp.width, vp.height);
  const std::string cachePath =
      isEpub
          ? epubSectionCachePath(provider->getEpub()->getCachePath(), request.targetSpine)
          : ([&]() {
              auto* fb2Provider = coreRef.content.asFb2();
              std::string fb2CachePath;
              if (type == ContentType::Fb2 &&
                  resolveFb2SectionContext(fb2Provider, config, request.targetSpine, &fb2CachePath, nullptr, nullptr)) {
                return fb2CachePath;
              }
              return contentCachePath(coreRef.content.cacheDir(), config.fontId);
            })();
  const std::string imageCachePath =
      isEpub && coreRef.settings.showImages ? (provider->getEpub()->getCachePath() + "/images") : "";

  const bool acquired = cacheController_.withWorkerResources("page-fill", [&](auto&) {
    const int parserTargetSpine =
        (isEpub || (type == ContentType::Fb2 && fb2UsesSectionNavigation(coreRef.content.asFb2())))
            ? request.targetSpine
            : 0;
    if (!parser_ || parserSpineIndex_ != parserTargetSpine) {
      if (isEpub && promoteLookaheadParser(request.targetSpine)) {
        // Promoted existing resumable parser for this spine.
      } else if (isEpub) {
        parser_.reset(
            new EpubChapterParser(provider->getEpubShared(), request.targetSpine, renderer_, config, imageCachePath, true));
        parserSpineIndex_ = request.targetSpine;
      } else if (type == ContentType::Fb2) {
        auto* fb2Provider = coreRef.content.asFb2();
        uint32_t startOffset = 0;
        int startingSectionIndex = 0;
        if (resolveFb2SectionContext(fb2Provider, config, request.targetSpine, nullptr, &startOffset,
                                     &startingSectionIndex)) {
          parser_.reset(new Fb2Parser(contentPath_, renderer_, config, startOffset, startingSectionIndex, true));
          parserSpineIndex_ = request.targetSpine;
        } else {
          parser_.reset(new Fb2Parser(contentPath_, renderer_, config));
          parserSpineIndex_ = 0;
        }
      } else if (type == ContentType::Markdown) {
        parser_.reset(new MarkdownParser(contentPath_, renderer_, config));
        parserSpineIndex_ = 0;
      } else if (type == ContentType::Html) {
        parser_.reset(new HtmlParser(contentPath_, coreRef.content.cacheDir(), renderer_, config));
        parserSpineIndex_ = 0;
      } else if (type == ContentType::Txt) {
        parser_.reset(new PlainTextParser(contentPath_, renderer_, config));
        parserSpineIndex_ = 0;
      }
    }

    if (!pageCache_ || pageCache_->path() != cachePath) {
      pageCache_.reset(new PageCache(cachePath));
    }

    bool cacheLoaded = pageCache_->load(config);
    auto pageResolved = [&]() -> bool {
      if (request.requireComplete) {
        return !pageCache_->isPartial() && pageCache_->pageCount() > 0;
      }
      return request.targetPage >= 0 && request.targetPage < static_cast<int>(pageCache_->pageCount());
    };

    auto nextBatchSize = [&]() -> uint16_t {
      const uint16_t defaultBatch =
          (!isEpub && type == ContentType::Fb2 && parser_ && !parser_->canResume())
              ? reader::kNonResumableCacheBatchPages
              : PageCache::DEFAULT_CACHE_CHUNK;
      if (request.requireComplete) {
        return defaultBatch;
      }
      const int remainingPages = (request.targetPage + 1) - static_cast<int>(pageCache_->pageCount());
      if (isEpub) {
        const int desiredPages =
            std::max(1, remainingPages + static_cast<int>(reader::kEpubInteractivePageFillHeadroomPages));
        return static_cast<uint16_t>(std::min<int>(desiredPages, defaultBatch));
      }
      if (remainingPages <= 1) {
        return 1;
      }
      return static_cast<uint16_t>(std::min<int>(remainingPages, defaultBatch));
    };

    if (pageResolved()) {
      return;
    }

    while (!(shouldAbort && shouldAbort())) {
      const uint16_t batchSize = nextBatchSize();
      const bool success = cacheLoaded ? pageCache_->extend(*parser_, batchSize, shouldAbort)
                                       : pageCache_->create(*parser_, config, batchSize, 0, shouldAbort);
      if (!success) {
        LOG_ERR(TAG, "[ASYNC] PageFill worker failed spine=%d page=%d", request.targetSpine, request.targetPage);
        break;
      }

      saveAnchorMap(*parser_, cachePath);
      if (pageResolved() || !pageCache_->isPartial()) {
        break;
      }

      cacheLoaded = true;
      vTaskDelay(1 / portTICK_PERIOD_MS);
    }
  });

  if (!acquired) {
    LOG_ERR(TAG, "[OWNERSHIP] PageFill worker could not acquire document resources");
  }
}

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
      LOG_ERR(TAG, "[ASYNC] arm TOC jump toc=%d spine=%d anchor=%s", tocIndex, targetSpine, tocItem.anchor.c_str());
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
        LOG_ERR(TAG, "[ASYNC] arm FB2 TOC jump toc=%d anchor=%s hintPage=%d", tocIndex, anchor.c_str(),
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
    LOG_ERR(TAG, "[ASYNC] TOC worker not started active=%u running=%u state=%d",
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
  LOG_ERR(TAG, "[ASYNC] Start TOC worker spine=%d anchor=%s hint=%d attempt=%u", targetSpine, targetAnchor.c_str(),
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
    LOG_ERR(TAG, "[ASYNC] TOC worker start failed spine=%d state=%d", targetSpine, static_cast<int>(workerState()));
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
    LOG_ERR(TAG,
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
    LOG_ERR(TAG, "[ASYNC] Page worker not started active=%u running=%u state=%d",
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

  const ReaderHeapState heapBefore = readReaderHeapState();
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
  const ReaderHeapState heapAfter = readReaderHeapState();
  LOG_ERR(TAG, "[ASYNC] page-fill memory trim free=%u->%u largest=%u->%u",
          static_cast<unsigned>(heapBefore.freeBytes), static_cast<unsigned>(heapAfter.freeBytes),
          static_cast<unsigned>(heapBefore.largestBlock), static_cast<unsigned>(heapAfter.largestBlock));

  pendingEpubPageLoadRetryCount_++;
  LOG_ERR(TAG, "[ASYNC] Start page worker spine=%d page=%d complete=%u attempt=%u", targetSpine, targetPage,
          static_cast<unsigned>(requireComplete), static_cast<unsigned>(pendingEpubPageLoadRetryCount_));

  reader::ReaderAsyncJobsController::PageFillRequest request;
  request.targetSpine = targetSpine;
  request.targetPage = targetPage;
  request.requireComplete = requireComplete;
  if (!asyncJobs_.queuePageFillWork(request)) {
    if (pendingEpubPageLoadRetryCount_ > 0) {
      pendingEpubPageLoadRetryCount_--;
    }
    LOG_ERR(TAG, "[ASYNC] Page worker start failed spine=%d page=%d state=%d", targetSpine, targetPage,
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
    LOG_ERR(TAG,
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
        LOG_ERR(TAG, "[ASYNC] page pending resolved complete spine=%d pages=%u", currentSpineIndex_,
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
      LOG_ERR(TAG, "[ASYNC] page pending resolved target spine=%d page=%d cachedPages=%u", currentSpineIndex_,
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

  if ((!pageCache_ || pageCache_->isPartial()) && pendingEpubPageLoadRetryCount_ < kPendingEpubPageLoadMaxRetries) {
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
    LOG_ERR(TAG, "Falling back to nearest cached page spine=%d page=%d", currentSpineIndex_, currentSectionPage_);
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

  if (partialRefresh) {
    renderer_.displayBuffer();
  } else {
    renderer_.displayBuffer();
  }
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

  if (partialRefresh) {
    renderer_.displayBuffer();
  } else {
    renderer_.displayBuffer();
  }
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
