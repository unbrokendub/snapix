#include "ReaderState.h"

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

#include <algorithm>

#include "../FontManager.h"
#include "../core/Core.h"
#include "ThemeManager.h"
#include "reader/ReaderStateInternal.h"
#include "reader/ReaderSupport.h"

#define TAG "READER"

namespace papyrix {
using reader::contentCachePath;
using reader::epubSectionCachePath;
using reader::fb2SectionCachePath;
using reader::kDefaultCacheBatchPages;
using reader::kNonResumableCacheBatchPages;

namespace {

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

}  // namespace

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
  LOG_INF(TAG, "[CACHE] worker state trigger=%s reason=%s candidate=%d farSweep=%u state=running", request.trigger,
          backgroundCacheWakeReasonToString(request.plan.reason), request.plan.candidateSpine,
          static_cast<unsigned>(request.plan.allowFarSweep));

  bool workerDidBackgroundWork = false;
  const bool acquired = cacheController_.withWorkerResources("background-cache", [&](auto&) {
    // Final heap check on the worker thread: the plan was made on the main
    // thread, but heap may have changed since.  Allocating a new
    // ContentParser here when heap is critical risks corrupting SdFat.
    {
      const auto workerHeap = reader::readHeapState();
      if (reader::isHeapCritical(workerHeap)) {
        LOG_INF(TAG, "[CACHE] worker aborted: heap critical (free=%u largest=%u)",
                static_cast<unsigned>(workerHeap.freeBytes), static_cast<unsigned>(workerHeap.largestBlock));
        return;
      }
    }

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
      LOG_INF(TAG, "[CACHE] scheduled repaint after background cache update spine=%d page=%d",
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
    LOG_INF(TAG, "[OWNERSHIP] background cache worker could not acquire document resources");
    return;
  }

  LOG_INF(TAG, "[CACHE] worker state trigger=%s reason=%s candidate=%d farSweep=%u state=%s didWork=%u stopRequested=%u",
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

  LOG_INF(TAG, "[ASYNC] TOC worker entered spine=%d anchor=%s", request.targetSpine, targetAnchor.c_str());

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

    LOG_INF(TAG, "[ASYNC] FB2 TOC build anchor=%s current=%u target=%u hint=%d resumable=%u", targetAnchor.c_str(),
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
        LOG_INF(TAG,
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
              // resolveFb2SectionContext can fail when meta.bin is missing even though
              // the section cache file exists on disk.  Probe the expected path directly
              // so we reuse the existing cache instead of falling back to a full-book
              // cache path that will fail to create and leave the reader stuck.
              if (type == ContentType::Fb2 && fb2Provider && fb2Provider->getFb2()) {
                std::string sectionPath = fb2SectionCachePath(
                    fb2Provider->getFb2()->getCachePath(), config.fontId, request.targetSpine);
                if (SdMan.exists(sectionPath.c_str())) {
                  return sectionPath;
                }
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
    LOG_INF(TAG, "[OWNERSHIP] PageFill worker could not acquire document resources");
  }
}

}  // namespace papyrix
