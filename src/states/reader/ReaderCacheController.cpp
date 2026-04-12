#include "ReaderCacheController.h"

#include <Arduino.h>
#include <EpubChapterParser.h>
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

#include <algorithm>
#include <cstring>

#include "../../ThemeManager.h"
#include "../../content/Fb2Provider.h"
#include "../../core/Core.h"

#define TAG "RDR_CACHE"

namespace papyrix::reader {

namespace {

constexpr size_t kEpubNearPrefetchMinFreeBytes = 32 * 1024;
constexpr size_t kEpubNearPrefetchMinLargestBlock = 18 * 1024;
constexpr size_t kEpubResidentWarmMinFreeBytes = 44 * 1024;
constexpr size_t kEpubResidentWarmMinLargestBlock = 20 * 1024;
constexpr size_t kEpubFarSweepMinFreeBytes = 56 * 1024;
constexpr size_t kEpubFarSweepMinLargestBlock = 24 * 1024;

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
    *cachePath = fb2SectionCachePath(provider->getFb2()->getCachePath(), config.fontId, tocIndex);
  }
  if (startOffset) {
    *startOffset = item.sourceOffset;
  }
  if (startingSectionIndex) {
    *startingSectionIndex = item.sectionIndex;
  }
  return true;
}

std::string activeContentCachePath(Core& core, const RenderConfig& config, const int spineIndex) {
  const ContentType type = core.content.metadata().type;
  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (provider && provider->getEpub()) {
      return epubSectionCachePath(provider->getEpub()->getCachePath(), spineIndex);
    }
  } else if (type == ContentType::Fb2) {
    auto* provider = core.content.asFb2();
    std::string cachePath;
    if (resolveFb2SectionContext(provider, config, spineIndex, &cachePath, nullptr, nullptr)) {
      return cachePath;
    }
  }
  return contentCachePath(core.content.cacheDir(), config.fontId);
}

bool canRunEpubNearPrefetch(const HeapState& heap) {
  return heap.freeBytes >= kEpubNearPrefetchMinFreeBytes && heap.largestBlock >= kEpubNearPrefetchMinLargestBlock;
}

bool canRunEpubResidentWarm(const HeapState& heap) {
  return heap.freeBytes >= kEpubResidentWarmMinFreeBytes &&
         heap.largestBlock >= kEpubResidentWarmMinLargestBlock;
}

bool canRunEpubFarSweep(const HeapState& heap) {
  return heap.freeBytes >= kEpubFarSweepMinFreeBytes && heap.largestBlock >= kEpubFarSweepMinLargestBlock;
}

}  // namespace

ReaderCacheController::ReaderCacheController(GfxRenderer& renderer, PositionRefs position)
    : position_(position), resources_(renderer) {}

void ReaderCacheController::setContentPath(const char* path) { contentPath_ = path ? path : ""; }

void ReaderCacheController::resetSession() {
  clearPagePrefetch();
  clearLookaheadParser();
  resetBackgroundPrefetchState();
  invalidateAnchorMapCache();
  auto& state = resources_.unsafeState();
  state.pageCache.reset();
  state.parser.reset();
  state.parserSpineIndex = -1;
  state.lookaheadParser.reset();
  state.lookaheadParserSpineIndex = -1;
  thumbnailDone_ = false;
  lastIdleBackgroundKickMs_ = 0;
  lastReaderInteractionMs_ = 0;
}

void ReaderCacheController::clearPagePrefetch() {
  if (auto* cache = pageCache()) {
    cache->clearResidentPages();
  }
  warmedNextPage_.clear();
  warmedNextNextPage_.clear();
  renderOverridePage_.clear();
}

void ReaderCacheController::clearLookaheadParser() {
  auto& state = resources_.unsafeState();
  state.lookaheadParser.reset();
  state.lookaheadParserSpineIndex = -1;
}

void ReaderCacheController::resetBackgroundPrefetchState() {
  backgroundNearPrefetchComplete_ = false;
  backgroundPrefetchBlockedSpine_ = -1;
  backgroundPrefetchBlockedFreeBytes_ = 0;
  backgroundPrefetchBlockedLargestBlock_ = 0;
  backgroundPrefetchBlockedMs_ = 0;
  backgroundPrefetchCursorSpine_ = -1;
  backgroundPrefetchSweepComplete_ = false;
}

const char* ReaderCacheController::backgroundCacheWakeReasonToString(const BackgroundCacheWakeReason reason) {
  switch (reason) {
    case BackgroundCacheWakeReason::None:
      return "none";
    case BackgroundCacheWakeReason::Thumbnail:
      return "thumbnail";
    case BackgroundCacheWakeReason::CurrentCacheMissing:
      return "current-cache-missing";
    case BackgroundCacheWakeReason::CurrentCachePartial:
      return "current-cache-partial";
    case BackgroundCacheWakeReason::CurrentCachePartialWaiting:
      return "current-cache-partial-waiting";
    case BackgroundCacheWakeReason::NearPrefetchReady:
      return "near-prefetch-ready";
    case BackgroundCacheWakeReason::FarPrefetchReady:
      return "far-prefetch-ready";
    case BackgroundCacheWakeReason::BlockedByHeap:
      return "blocked-by-heap";
  }
  return "unknown";
}

bool ReaderCacheController::promoteLookaheadParser(const int targetSpine) {
  auto& state = resources_.unsafeState();
  if (!state.lookaheadParser || state.lookaheadParserSpineIndex != targetSpine) {
    return false;
  }

  state.parser = std::move(state.lookaheadParser);
  state.parserSpineIndex = state.lookaheadParserSpineIndex;
  state.lookaheadParserSpineIndex = -1;
  return true;
}

bool ReaderCacheController::hasRenderOverride(const int spine, const int sectionPage) const {
  return renderOverridePage_.matches(spine, sectionPage);
}

WarmPageSlot ReaderCacheController::takeRenderOverride() {
  WarmPageSlot slot = renderOverridePage_;
  renderOverridePage_.clear();
  return slot;
}

void ReaderCacheController::clearRenderOverride() { renderOverridePage_.clear(); }

void ReaderCacheController::setRenderOverride(WarmPageSlot slot) { renderOverridePage_ = std::move(slot); }

void ReaderCacheController::consumeWarmForwardPage() {
  renderOverridePage_ = warmedNextPage_;
  warmedNextPage_ = warmedNextNextPage_;
  warmedNextNextPage_.clear();
}

bool ReaderCacheController::hasPageCache() const { return resources_.unsafeState().pageCache != nullptr; }

PageCache* ReaderCacheController::pageCache() { return resources_.unsafeState().pageCache.get(); }

const PageCache* ReaderCacheController::pageCache() const { return resources_.unsafeState().pageCache.get(); }

bool ReaderCacheController::pageCachePartial() const { return pageCache() ? pageCache()->isPartial() : false; }

size_t ReaderCacheController::pageCacheCount() const { return pageCache() ? pageCache()->pageCount() : 0; }

bool ReaderCacheController::withForegroundResources(
    const char* reason, const std::function<void(ReaderDocumentResources::Session&)>& fn) {
  auto session = resources_.acquireForeground(reason);
  if (!session) {
    return false;
  }
  fn(session);
  return true;
}

bool ReaderCacheController::withWorkerResources(
    const char* reason, const std::function<void(ReaderDocumentResources::Session&)>& fn) {
  auto session = resources_.acquireWorker(reason);
  if (!session) {
    return false;
  }
  fn(session);
  return true;
}

uint16_t ReaderCacheController::nonEpubBatchSizeFor(const ContentType type, const ContentParser& parser) const {
  if (type == ContentType::Fb2 && !parser.canResume()) {
    return kNonResumableCacheBatchPages;
  }
  return kDefaultCacheBatchPages;
}

void ReaderCacheController::prefetchAdjacentPage(Core& core) {
  warmedNextPage_.clear();
  warmedNextNextPage_.clear();

  auto& state = resources_.unsafeState();
  if (!state.pageCache || position_.currentSectionPage < 0) return;

  const int pageCount = static_cast<int>(state.pageCache->pageCount());
  if (pageCount <= 1) return;

  const ContentType type = core.content.metadata().type;
  const HeapState heap = readHeapState();
  const bool allowEpubNearPrefetch = (type == ContentType::Epub) ? canRunEpubNearPrefetch(heap) : !isHeapCritical(heap);
  if (!allowEpubNearPrefetch) {
    state.pageCache->clearResidentPages();
    perfLog(TAG, "reader-prefetch-skip", perfMsNow(), "(reason=heap-critical free=%u largest=%u)",
            static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlock));
    return;
  }

  int direction = 1;
  if (position_.lastRenderedSpineIndex == position_.currentSpineIndex &&
      position_.lastRenderedSectionPage > position_.currentSectionPage) {
    direction = -1;
  }

  const int nextPage = position_.currentSectionPage + direction;
  if (nextPage < 0 || nextPage >= pageCount) return;

  const bool allowResidentWarm = (type == ContentType::Epub) ? canRunEpubResidentWarm(heap) : !isHeapTight(heap);
  const uint8_t prefetchSpan = (type == ContentType::Epub && allowResidentWarm) ? 2 : 1;
  const uint32_t prefetchMs = perfMsNow();
  state.pageCache->prefetchWindow(static_cast<uint16_t>(position_.currentSectionPage), direction, prefetchSpan);
  perfLog(TAG, "page-prefetch", prefetchMs, "(center=%d dir=%d resident=%u)", position_.currentSectionPage, direction,
          static_cast<unsigned>(state.pageCache->residentPageCount()));

  if (!allowResidentWarm) return;

  const Theme& theme = THEME_MANAGER.current();
  const int fontId = core.settings.getReaderFontId(theme);
  for (int delta = 1; delta <= 2; ++delta) {
    const int forwardPage = position_.currentSectionPage + delta;
    if (forwardPage < 0 || forwardPage >= pageCount) {
      continue;
    }

    std::shared_ptr<Page> warmedPage = state.pageCache->loadPage(static_cast<uint16_t>(forwardPage));
    if (!warmedPage) {
      continue;
    }

    if (delta == 1) {
      warmedPage->warmGlyphs(resources_.renderer(), fontId);
      warmedNextPage_.spineIndex = position_.currentSpineIndex;
      warmedNextPage_.sectionPage = forwardPage;
      warmedNextPage_.pageCount = state.pageCache->pageCount();
      warmedNextPage_.isPartial = state.pageCache->isPartial();
      warmedNextPage_.page = warmedPage;
    } else {
      warmedNextNextPage_.spineIndex = position_.currentSpineIndex;
      warmedNextNextPage_.sectionPage = forwardPage;
      warmedNextNextPage_.pageCount = state.pageCache->pageCount();
      warmedNextNextPage_.isPartial = state.pageCache->isPartial();
      warmedNextNextPage_.page = warmedPage;
    }
  }
}

void ReaderCacheController::saveAnchorMap(const ContentParser& parser, const std::string& cachePath) {
  const auto& anchorMap = parser.getAnchorMap();
  if (anchorMap.empty()) return;

  FsFile file;
  const std::string mapPath = cachePath + ".anchors";
  if (!SdMan.openFileForWrite("CACHE", mapPath, file)) {
    return;
  }

  const uint16_t count = static_cast<uint16_t>(anchorMap.size());
  serialization::writePod(file, count);
  for (const auto& [id, page] : anchorMap) {
    serialization::writeString(file, id);
    serialization::writePod(file, page);
  }
  file.close();
}

int ReaderCacheController::loadAnchorPage(const std::string& cachePath, const std::string& anchor) {
  const auto anchors = loadAnchorMap(cachePath);
  for (const auto& entry : anchors) {
    if (entry.first == anchor) {
      return entry.second;
    }
  }
  return -1;
}

std::vector<std::pair<std::string, uint16_t>> ReaderCacheController::loadAnchorMap(const std::string& cachePath) {
  std::vector<std::pair<std::string, uint16_t>> anchors;
  FsFile file;
  const std::string mapPath = cachePath + ".anchors";
  if (!SdMan.openFileForRead("CACHE", mapPath, file)) {
    return anchors;
  }

  uint16_t count = 0;
  serialization::readPod(file, count);
  anchors.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    std::string id;
    uint16_t page = 0;
    if (!serialization::readString(file, id)) break;
    serialization::readPod(file, page);
    anchors.emplace_back(std::move(id), page);
  }
  file.close();
  return anchors;
}

const std::vector<std::pair<std::string, uint16_t>>& ReaderCacheController::getCachedAnchorMap(
    const std::string& cachePath, const int spineIndex) {
  if (cachedAnchorMapSpine_ == spineIndex && cachedAnchorMapPath_ == cachePath) {
    return cachedAnchorMap_;
  }

  cachedAnchorMap_ = loadAnchorMap(cachePath);
  cachedAnchorMapPath_ = cachePath;
  cachedAnchorMapSpine_ = spineIndex;
  return cachedAnchorMap_;
}

void ReaderCacheController::invalidateAnchorMapCache() {
  cachedAnchorMap_.clear();
  cachedAnchorMap_.shrink_to_fit();
  cachedAnchorMapPath_.clear();
  cachedAnchorMapSpine_ = -1;
}

int ReaderCacheController::calcFirstContentSpine(const bool hasCover, const int textStartIndex, const size_t spineCount) {
  if (spineCount == 0) return 0;
  if (textStartIndex > 0 && textStartIndex < static_cast<int>(spineCount)) return textStartIndex;
  if (hasCover && spineCount > 1) return 1;
  return 0;
}

void ReaderCacheController::createOrExtendCacheImpl(ContentParser& parser, const std::string& cachePath,
                                                    const RenderConfig& config, const uint16_t batchSize) {
  auto& state = resources_.unsafeState();
  const uint16_t targetPage = static_cast<uint16_t>(std::max(position_.currentSectionPage, 0));
  const bool needLastPage = position_.currentSectionPage == INT16_MAX;

  bool cacheLoaded = false;
  if (state.pageCache && state.pageCache->path() != cachePath) {
    state.pageCache.reset();
  }

  if (state.pageCache) {
    cacheLoaded = state.pageCache->pageCount() > 0;
    if (!cacheLoaded) {
      cacheLoaded = state.pageCache->load(config);
      if (!cacheLoaded) {
        LOG_ERR(TAG, "[CACHE] foreground cache miss path=%s reason=load-failed", cachePath.c_str());
        state.pageCache.reset();
      }
    }
  }

  if (cacheLoaded && state.pageCache) {
    const bool needsExtend = needLastPage ? state.pageCache->isPartial() : state.pageCache->needsExtension(targetPage);
    const bool pageAlreadyAvailable =
        !needLastPage && targetPage < static_cast<uint16_t>(state.pageCache->pageCount());

    if (pageAlreadyAvailable && !needsExtend) {
      return;
    }

    if (state.pageCache->isPartial()) {
      state.pageCache->clearResidentPages();
      const uint16_t effectiveBatch = batchSize == 0 ? kDefaultCacheBatchPages : batchSize;
      if (state.pageCache->extend(parser, effectiveBatch)) {
        saveAnchorMap(parser, cachePath);
        return;
      }
      LOG_ERR(TAG, "[CACHE] foreground extend failed path=%s targetPage=%u lastPage=%u", cachePath.c_str(),
              static_cast<unsigned>(targetPage), static_cast<unsigned>(needLastPage));
      state.pageCache.reset();
    } else if (pageAlreadyAvailable || needLastPage) {
      return;
    }
  }

  if (!state.pageCache) {
    state.pageCache.reset(new PageCache(cachePath));
  }
  const uint16_t effectiveBatch = batchSize == 0 ? kDefaultCacheBatchPages : batchSize;
  if (state.pageCache->create(parser, config, effectiveBatch)) {
    saveAnchorMap(parser, cachePath);
    return;
  }

  state.pageCache.reset();
}

void ReaderCacheController::backgroundCacheImpl(ContentParser& parser, const std::string& cachePath,
                                                const RenderConfig& config, const int sectionPageHint,
                                                const AbortCallback& shouldAbort, const bool forceExtendPartial) {
  auto& state = resources_.unsafeState();
  const int safeSectionPage = sectionPageHint < 0 ? 0 : sectionPageHint;
  const bool reuseCurrentCache = state.pageCache && state.pageCache->path() == cachePath;
  if (!state.pageCache || !reuseCurrentCache) {
    state.pageCache.reset(new PageCache(cachePath));
  }

  bool loaded = false;
  if (reuseCurrentCache && state.pageCache) {
    loaded = state.pageCache->pageCount() > 0;
    if (!loaded) {
      LOG_ERR(TAG, "[CACHE] background same-cache skipped path=%s reason=active-cache-empty", cachePath.c_str());
      return;
    }
  } else if (state.pageCache) {
    loaded = state.pageCache->load(config);
  }
  bool needsExtend = loaded && (forceExtendPartial || state.pageCache->needsExtension(safeSectionPage));

  if (loaded && !needsExtend && !state.pageCache->isPartial()) {
    return;
  }

  if (loaded) {
    state.pageCache->clearResidentPages();
    if (reuseCurrentCache) {
      state.pageCache->trimResidentPages(safeSectionPage, 0, 2);
    }
  }

  if (shouldAbort && shouldAbort()) {
    if (!reuseCurrentCache) {
      state.pageCache.reset();
    }
    LOG_DBG(TAG, "Background cache aborted after setup%s", reuseCurrentCache ? " (kept active cache)" : "");
    return;
  }

  const uint16_t backgroundBatchSize =
      (reuseCurrentCache && loaded && state.pageCache && state.pageCache->isPartial() && parser.canResume())
          ? kEpubInteractiveHotExtendBatchPages
          : kDefaultCacheBatchPages;

  bool success = false;
  if (loaded && state.pageCache->isPartial()) {
    success = state.pageCache->extend(parser, backgroundBatchSize, shouldAbort);
  } else {
    success = state.pageCache->create(parser, config, backgroundBatchSize, 0, shouldAbort);
  }

  if (success && !(shouldAbort && shouldAbort())) {
    saveAnchorMap(parser, cachePath);
    return;
  }

  if (!reuseCurrentCache) {
    state.pageCache.reset();
  }
}

bool ReaderCacheController::shouldContinueIdleBackgroundCaching(Core& core) {
  const ContentType type = core.content.metadata().type;
  if (type == ContentType::Xtc) {
    return false;
  }
  if (!hasPageCache()) {
    return true;
  }

  if (pageCachePartial()) {
    const int activeSpine = position_.currentSectionPage < 0 ? calcFirstContentSpine(position_.hasCover, position_.textStartIndex, 0)
                                                             : position_.currentSpineIndex;
    RenderConfig config{};
    config.fontId = core.settings.getReaderFontId(THEME_MANAGER.current());
    const bool currentCacheMatchesActive = pageCache()->path() == activeContentCachePath(core, config, activeSpine);
    if (!currentCacheMatchesActive) {
      return true;
    }
    auto& state = resources_.unsafeState();
    const bool canHotExtend = ((state.parser && state.parserSpineIndex == activeSpine && state.parser->canResume()) ||
                               (state.lookaheadParser && state.lookaheadParserSpineIndex == activeSpine &&
                                state.lookaheadParser->canResume()));
    const bool nearTail = position_.currentSectionPage >= 0 &&
                          pageCache()->needsExtension(static_cast<uint16_t>(std::max(position_.currentSectionPage, 0)));
    if (type == ContentType::Epub && currentCacheMatchesActive) {
      return true;
    }
    return nearTail || (type == ContentType::Epub && canHotExtend);
  }

  return type == ContentType::Epub && !backgroundPrefetchSweepComplete_;
}

bool ReaderCacheController::prefetchNextEpubSpineCache(Core& core, const RenderConfig& config, const int activeSpineIndex,
                                                       const bool coverExists, const int textStartIndex,
                                                       const bool allowFarSweep, const AbortCallback& shouldAbort) {
  auto* provider = core.content.asEpub();
  if (!provider || !provider->getEpub()) {
    return false;
  }

  auto epub = provider->getEpubShared();
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) {
    return false;
  }

  const int firstContentSpine = calcFirstContentSpine(coverExists, textStartIndex, spineCount);
  const int nextSpine = std::max(activeSpineIndex + 1, firstContentSpine);
  if (nextSpine >= spineCount) {
    backgroundNearPrefetchComplete_ = true;
    backgroundPrefetchSweepComplete_ = true;
    return false;
  }

  bool didWork = false;
  const int prefetchScanStart = nextSpine;
  const int prefetchScanEnd = std::min(spineCount, allowFarSweep ? nextSpine + kEpubActivePrefetchAheadSpines : nextSpine + 1);
  const std::string imageCachePath = core.settings.showImages ? (epub->getCachePath() + "/images") : "";
  auto& state = resources_.unsafeState();

  for (int spine = prefetchScanStart; spine < prefetchScanEnd; ++spine) {
    if (spine > prefetchScanStart) {
      const HeapState heap = readHeapState();
      if (allowFarSweep && !canRunEpubFarSweep(heap)) {
        backgroundPrefetchBlockedSpine_ = spine;
        backgroundPrefetchBlockedFreeBytes_ = heap.freeBytes;
        backgroundPrefetchBlockedLargestBlock_ = heap.largestBlock;
        backgroundPrefetchCursorSpine_ = spine;
        backgroundPrefetchSweepComplete_ = false;
        LOG_ERR(TAG, "[CACHE] background prefetch blocked spine=%d farSweep=%u free=%u largest=%u cursor=%d active=%d",
                spine, static_cast<unsigned>(allowFarSweep), static_cast<unsigned>(heap.freeBytes),
                static_cast<unsigned>(heap.largestBlock), backgroundPrefetchCursorSpine_, activeSpineIndex);
        return didWork;
      }
    }

    if (spine > prefetchScanStart && shouldAbort && shouldAbort()) {
      return didWork;
    }

    const std::string nextCachePath = epubSectionCachePath(epub->getCachePath(), spine);
    PageCache nextCache(nextCachePath);
    if (nextCache.load(config) && !nextCache.isPartial()) {
      backgroundNearPrefetchComplete_ = (spine == nextSpine);
      if (allowFarSweep && spine == prefetchScanEnd - 1) {
        backgroundPrefetchSweepComplete_ = true;
      }
      continue;
    }

    ContentParser* parser = nullptr;
    const bool usePersistentLookahead = (state.lookaheadParser && state.lookaheadParserSpineIndex == spine) || spine == nextSpine;
    if (usePersistentLookahead) {
      if (!state.lookaheadParser || state.lookaheadParserSpineIndex != spine) {
        state.lookaheadParser.reset(new EpubChapterParser(epub, spine, resources_.renderer(), config, imageCachePath, true));
        state.lookaheadParserSpineIndex = spine;
      }
      parser = state.lookaheadParser.get();
    } else {
      if (shouldAbort && shouldAbort()) {
        return didWork;
      }
      auto transientParser = std::make_unique<EpubChapterParser>(epub, spine, resources_.renderer(), config, imageCachePath, true);
      if (nextCache.load(config) && nextCache.isPartial()) {
        if (nextCache.extend(*transientParser, kDefaultCacheBatchPages, shouldAbort)) {
          didWork = true;
        }
      } else {
        PageCache transientCache(nextCachePath);
        if (transientCache.create(*transientParser, config, kDefaultCacheBatchPages, 0, shouldAbort)) {
          didWork = true;
        }
      }
      continue;
    }

    if (!parser) {
      continue;
    }

    if (nextCache.load(config) && nextCache.isPartial()) {
      if (nextCache.extend(*parser, kDefaultCacheBatchPages, shouldAbort)) {
        didWork = true;
      }
    } else {
      PageCache buildCache(nextCachePath);
      if (buildCache.create(*parser, config, kDefaultCacheBatchPages, 0, shouldAbort)) {
        didWork = true;
      }
    }
    backgroundNearPrefetchComplete_ = (spine == nextSpine) && !nextCache.isPartial();
  }

  backgroundPrefetchSweepComplete_ = allowFarSweep;
  return didWork;
}

BackgroundCachePlan ReaderCacheController::planBackgroundCacheWork(Core& core) {
  BackgroundCachePlan plan;
  const ContentType type = core.content.metadata().type;

  if (type == ContentType::Xtc) {
    if (!thumbnailDone_) {
      plan.shouldStart = true;
      plan.reason = BackgroundCacheWakeReason::Thumbnail;
      plan.activeSpine = position_.currentSpineIndex;
    }
    return plan;
  }

  const int safeSectionPage = position_.currentSectionPage < 0 ? 0 : position_.currentSectionPage;
  const int activeSpine = (type == ContentType::Epub && position_.currentSectionPage == -1)
                              ? calcFirstContentSpine(position_.hasCover, position_.textStartIndex,
                                                      core.content.asEpub() ? core.content.asEpub()->getEpub()->getSpineItemsCount()
                                                                            : 0)
                              : position_.currentSpineIndex;

  plan.activeSpine = activeSpine;
  if (!pageCache()) {
    plan.shouldStart = true;
    plan.reason = BackgroundCacheWakeReason::CurrentCacheMissing;
    plan.candidateSpine = activeSpine;
    return plan;
  }

  if (pageCache()->isPartial()) {
    auto& state = resources_.unsafeState();
    RenderConfig config{};
    config.fontId = core.settings.getReaderFontId(THEME_MANAGER.current());
    const bool currentCacheMatchesActive = pageCache()->path() == activeContentCachePath(core, config, activeSpine);
    const bool currentPartialCanHotExtend =
        currentCacheMatchesActive &&
        ((state.parser && state.parserSpineIndex == activeSpine && state.parser->canResume()) ||
         (state.lookaheadParser && state.lookaheadParserSpineIndex == activeSpine && state.lookaheadParser->canResume()));
    const bool currentPartialNearTail =
        currentCacheMatchesActive && pageCache()->needsExtension(static_cast<uint16_t>(std::max(safeSectionPage, 0)));
    const uint32_t nowMs = millis();
    const bool recentReaderInteraction =
        lastReaderInteractionMs_ != 0 &&
        static_cast<uint32_t>(nowMs - lastReaderInteractionMs_) < kEpubActivePartialWorkerCooldownMs;
    const bool currentPageAtTail =
        currentCacheMatchesActive && pageCache()->pageCount() > 0 &&
        safeSectionPage >= static_cast<int>(pageCache()->pageCount()) - 1;
    const bool allowImmediateActivePartialWork = !recentReaderInteraction || currentPageAtTail;
    const bool currentEpubPartialNeedsCompletion = currentCacheMatchesActive && type == ContentType::Epub;

    plan.currentPartialCanHotExtend = currentPartialCanHotExtend;
    plan.currentPartialNearTail = currentPartialNearTail;

    if (!currentCacheMatchesActive) {
      plan.shouldStart = true;
      plan.reason = BackgroundCacheWakeReason::CurrentCachePartialWaiting;
      plan.candidateSpine = activeSpine;
      return plan;
    }

    // For EPUB, a partial cache means the current spine was not rebuilt to a
    // stable section boundary yet. Finishing that work is more valuable than
    // prefetching the next spine: it replaces transient placeholders/missing
    // images in the currently readable chapter and avoids wasting heap on
    // prefetch work that is likely to be preempted by the next page turn.
    if (currentEpubPartialNeedsCompletion) {
      if (!allowImmediateActivePartialWork) {
        plan.reason = BackgroundCacheWakeReason::CurrentCachePartialWaiting;
        plan.candidateSpine = activeSpine;
        return plan;
      }

      plan.shouldStart = true;
      plan.reason = BackgroundCacheWakeReason::CurrentCachePartial;
      plan.candidateSpine = activeSpine;
      return plan;
    }

    if (currentPartialNearTail || (type == ContentType::Epub && currentPartialCanHotExtend)) {
      if (!allowImmediateActivePartialWork) {
        plan.reason = BackgroundCacheWakeReason::CurrentCachePartialWaiting;
        plan.candidateSpine = activeSpine;
        return plan;
      }

      plan.shouldStart = true;
      plan.reason = BackgroundCacheWakeReason::CurrentCachePartial;
      plan.candidateSpine = activeSpine;
      return plan;
    }
  }

  if (type != ContentType::Epub) {
    return plan;
  }

  const HeapState heap = readHeapState();
  const bool allowNearPrefetch = canRunEpubNearPrefetch(heap);
  const bool idleEnoughForFarSweep =
      lastReaderInteractionMs_ != 0 && static_cast<uint32_t>(millis() - lastReaderInteractionMs_) >= kEpubDeepIdleSweepDelayMs;
  const bool heapAllowsFarSweep = canRunEpubFarSweep(heap);
  const bool allowFarSweep = idleEnoughForFarSweep && heapAllowsFarSweep;
  if (!allowNearPrefetch) {
    plan.reason = BackgroundCacheWakeReason::BlockedByHeap;
    plan.activeSpine = activeSpine;
    plan.candidateSpine = backgroundPrefetchCursorSpine_;
    plan.allowFarSweep = allowFarSweep;
    return plan;
  }

  if (!backgroundNearPrefetchComplete_) {
    plan.shouldStart = true;
    plan.reason = BackgroundCacheWakeReason::NearPrefetchReady;
    plan.candidateSpine = std::max(activeSpine + 1, 0);
    plan.allowFarSweep = false;
    return plan;
  }

  if (!idleEnoughForFarSweep) {
    return plan;
  }

  if (!heapAllowsFarSweep) {
    plan.reason = BackgroundCacheWakeReason::BlockedByHeap;
    plan.activeSpine = activeSpine;
    plan.candidateSpine = backgroundPrefetchCursorSpine_;
    plan.allowFarSweep = allowFarSweep;
    return plan;
  }

  if (allowFarSweep && !backgroundPrefetchSweepComplete_) {
    plan.shouldStart = true;
    plan.reason = BackgroundCacheWakeReason::FarPrefetchReady;
    plan.candidateSpine = std::max(backgroundPrefetchCursorSpine_, activeSpine + 1);
    plan.allowFarSweep = true;
    return plan;
  }

  return plan;
}

void ReaderCacheController::loadCacheFromDisk(Core& core, const Viewport& viewport) {
  const Theme& theme = THEME_MANAGER.current();
  const auto config = core.settings.getRenderConfig(theme, viewport.width, viewport.height);
  const ContentType type = core.content.metadata().type;
  std::string cachePath;

  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return;
    cachePath = epubSectionCachePath(provider->getEpub()->getCachePath(), position_.currentSpineIndex);
  } else if (type == ContentType::Fb2) {
    auto* provider = core.content.asFb2();
    if (!resolveFb2SectionContext(provider, config, position_.currentSpineIndex, &cachePath, nullptr, nullptr)) {
      cachePath = contentCachePath(core.content.cacheDir(), config.fontId);
    }
  } else {
    cachePath = contentCachePath(core.content.cacheDir(), config.fontId);
  }

  auto& state = resources_.unsafeState();
  state.pageCache.reset(new PageCache(cachePath));
  if (!state.pageCache->load(config)) {
    state.pageCache.reset();
  }
}

void ReaderCacheController::reloadCacheFromDisk(Core& core, const Viewport& viewport) {
  auto& state = resources_.unsafeState();
  state.pageCache.reset();
  loadCacheFromDisk(core, viewport);
}

void ReaderCacheController::createOrExtendCache(Core& core, const Viewport& viewport, uint16_t batchSize) {
  const Theme& theme = THEME_MANAGER.current();
  const auto config = core.settings.getRenderConfig(theme, viewport.width, viewport.height);
  const ContentType type = core.content.metadata().type;
  auto& state = resources_.unsafeState();

  std::string cachePath;
  ContentParser* parser = nullptr;

  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return;

    const int targetSpine = position_.currentSpineIndex;
    if (!state.parser || state.parserSpineIndex != targetSpine) {
      if (!promoteLookaheadParser(targetSpine)) {
        const std::string imageCachePath = core.settings.showImages ? (provider->getEpub()->getCachePath() + "/images") : "";
        state.parser.reset(new EpubChapterParser(provider->getEpubShared(), targetSpine, resources_.renderer(), config,
                                                 imageCachePath, false));
        state.parserSpineIndex = targetSpine;
      }
    }
    cachePath = epubSectionCachePath(provider->getEpub()->getCachePath(), targetSpine);
    parser = state.parser.get();
  } else if (type == ContentType::Markdown) {
    if (!state.parser) {
      state.parser.reset(new MarkdownParser(contentPath_, resources_.renderer(), config));
      state.parserSpineIndex = 0;
    }
    cachePath = contentCachePath(core.content.cacheDir(), config.fontId);
    parser = state.parser.get();
  } else if (type == ContentType::Fb2) {
    auto* provider = core.content.asFb2();
    uint32_t startOffset = 0;
    int startingSectionIndex = 0;
    if (resolveFb2SectionContext(provider, config, position_.currentSpineIndex, &cachePath, &startOffset,
                                 &startingSectionIndex)) {
      if (!state.parser || state.parserSpineIndex != position_.currentSpineIndex) {
        state.parser.reset(new Fb2Parser(contentPath_, resources_.renderer(), config, startOffset, startingSectionIndex,
                                         true));
        state.parserSpineIndex = position_.currentSpineIndex;
      }
    } else {
      if (!state.parser) {
        state.parser.reset(new Fb2Parser(contentPath_, resources_.renderer(), config));
        state.parserSpineIndex = 0;
      }
      cachePath = contentCachePath(core.content.cacheDir(), config.fontId);
    }
    parser = state.parser.get();
  } else if (type == ContentType::Html) {
    if (!state.parser) {
      state.parser.reset(new HtmlParser(contentPath_, core.content.cacheDir(), resources_.renderer(), config));
      state.parserSpineIndex = 0;
    }
    cachePath = contentCachePath(core.content.cacheDir(), config.fontId);
    parser = state.parser.get();
  } else if (type == ContentType::Txt) {
    if (!state.parser) {
      state.parser.reset(new PlainTextParser(contentPath_, resources_.renderer(), config));
      state.parserSpineIndex = 0;
    }
    cachePath = contentCachePath(core.content.cacheDir(), config.fontId);
    parser = state.parser.get();
  }

  if (!parser || cachePath.empty()) {
    return;
  }

  const uint16_t effectiveBatch =
      batchSize == 0 ? (type == ContentType::Epub ? kDefaultCacheBatchPages : nonEpubBatchSizeFor(type, *parser))
                     : batchSize;
  createOrExtendCacheImpl(*parser, cachePath, config, effectiveBatch);
}

}  // namespace papyrix::reader
