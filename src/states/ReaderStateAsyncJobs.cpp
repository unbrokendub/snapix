#include "ReaderState.h"

#include <Epub/PendingImageDecode.h>  // v2.0.83: async EPUB image decode queue
#include <EpubChapterParser.h>
#include <Fb2.h>
#include <Fb2Parser.h>
#include <GfxRenderer.h>
#include <LittleFS.h>  // v2.0.61: anchors file moved to LittleFS
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

namespace snapix {
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
    //
    // v2.0.67: switched from isHeapCritical (28K free / 10K largest) to
    // isHeapCriticalForHotExtend (15K free / 6K largest).  The strict
    // gate stopped ALL background cache work in mid-session reading
    // where heap is fragmented but workable.  Hot extend's transient
    // working set is ~5-10 KB; the looser gate matches that.  Cold
    // rebuild has its own stricter pre-flight inside PageCache::extend
    // (25 KB largest / 50 KB free) so it's still gated independently.
    {
      const auto workerHeap = reader::readHeapState();
      if (reader::isHeapCriticalForHotExtend(workerHeap)) {
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
            auto* epubParser = new EpubChapterParser(provider->getEpubShared(), activeSpineForCache, renderer_, config,
                                                     imageCachePath, true);
            // v2.0.131 — plumb real viewport margins so R4.c .idx
            // build uses the same paginator config as the render path.
            epubParser->setStreamingViewport(vp.marginTop, vp.marginBottom, vp.marginLeft, vp.marginRight);
            parser_.reset(epubParser);
            parserSpineIndex_ = activeSpineForCache;
          }
        }
      }
    } else if (type == ContentType::Markdown && !(shouldAbort && shouldAbort())) {
      cachePath = contentCachePath(coreRef.content.cacheDir(), config.fontId);
      if (!parser_) {
        // v2.0.192 — route through Markdown's effective content path so
        // cp1251 sources read from the UTF-8 cache (LittleFS) instead
        // of the raw SD file.  Mirrors the v2.0.189 PlainTextParser fix.
        const auto* mdProv = coreRef.content.asMarkdown();
        const Markdown* md = mdProv ? mdProv->getMarkdown() : nullptr;
        const std::string parserPath = md ? md->getEffectiveContentPath() : std::string(contentPath_);
        const bool useLfs = md ? md->isContentOnLittleFs() : false;
        parser_.reset(new MarkdownParser(parserPath, renderer_, config, useLfs));
        parserSpineIndex_ = 0;
      }
    } else if (type == ContentType::Fb2 && !(shouldAbort && shouldAbort())) {
      auto* fb2Provider = coreRef.content.asFb2();
      uint32_t startOffset = 0;
      uint32_t endOffset = 0;
      int startingSectionIndex = 0;
      if (resolveFb2SectionContext(fb2Provider, config, activeSpineForCache, &cachePath, &startOffset,
                                   &startingSectionIndex, &endOffset)) {
        if (!parser_ || parserSpineIndex_ != activeSpineForCache) {
          auto* fb2Parser = new Fb2Parser(contentPath_, renderer_, config, startOffset, startingSectionIndex, true, endOffset);
          if (fb2Provider && fb2Provider->getFb2()) fb2Parser->setFb2(fb2Provider->getFb2());
          // v2.0.131 — plumb real viewport margins (see EPUB site above).
          fb2Parser->setStreamingViewport(vp.marginTop, vp.marginBottom, vp.marginLeft, vp.marginRight);
          parser_.reset(fb2Parser);
          parserSpineIndex_ = activeSpineForCache;
        }
      } else {
        cachePath = contentCachePath(coreRef.content.cacheDir(), config.fontId);
        if (!parser_) {
          auto* fb2Parser = new Fb2Parser(contentPath_, renderer_, config);
          if (fb2Provider && fb2Provider->getFb2()) fb2Parser->setFb2(fb2Provider->getFb2());
          // v2.0.131 — plumb real viewport margins (see EPUB site above).
          fb2Parser->setStreamingViewport(vp.marginTop, vp.marginBottom, vp.marginLeft, vp.marginRight);
          parser_.reset(fb2Parser);
          parserSpineIndex_ = 0;
        }
      }
    } else if (type == ContentType::Html && !(shouldAbort && shouldAbort())) {
      // v2.0.162 — standalone HTML reading deleted along with the
      // legacy ChapterHtmlSlimParser pipeline.  Opening an .html file
      // now results in no parser → no pages → reader error.  Was a
      // niche feature; restoring it would require running the file
      // through the markerize pipeline (HtmlStripper directly on the
      // SD file instead of a ZIP stream).
    } else if (type == ContentType::Txt && !(shouldAbort && shouldAbort())) {
      cachePath = contentCachePath(coreRef.content.cacheDir(), config.fontId);
      if (!parser_) {
        // v2.0.189 — route the parser through the Txt object's
        // effective content path so cp1251 sources read from the
        // UTF-8 cache (LittleFS) instead of the raw cp1251 SD file.
        // For UTF-8/ASCII sources these fall back to contentPath_ +
        // SD (i.e., behaviour identical to v2.0.188 and earlier).
        const auto* txtProv = coreRef.content.asTxt();
        const Txt* txt = txtProv ? txtProv->getTxt() : nullptr;
        const std::string parserPath = txt ? txt->getEffectiveContentPath() : std::string(contentPath_);
        const bool useLfs = txt ? txt->isContentOnLittleFs() : false;
        parser_.reset(new PlainTextParser(parserPath, renderer_, config, useLfs));
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
            // v2.0.131 — pass vp so lookahead parsers get real viewport margins.
            cacheController_.prefetchNextEpubSpineCache(coreRef, config, activeSpineForCache, request.position.hasCover,
                                                        request.position.textStartIndex, request.plan.allowFarSweep,
                                                        shouldAbort, vp) ||
            didBackgroundWork;
      } else {
        cacheController_.resetBackgroundPrefetchState();
      }
    }

    // FB2 inline-image decode pass.  Fb2Parser registers <image>s in fast mode
    // (header peek only) so the page-turn doesn't stall behind a 3-8 s JPEG
    // decode.  We pick up the resulting pending/<id>.{jpg,png} files here and
    // materialise them into BMPs.  Each decoded image triggers a one-shot
    // repaint so the placeholder is replaced once the BMP exists.
    //
    // Step A (NEW): retryDeferredImages re-streams base64 for any binary id
    // whose previous fast-mode call was aborted by a page-turn cancel.  This
    // is what brings the placeholder ImageBlocks (registered with default
    // dims by cacheImage's abort branch) back to life — without it, those
    // placeholders would remain stuck forever on "Loading image...".
    if (type == ContentType::Fb2 && !(shouldAbort && shouldAbort())) {
      auto* fb2Provider = coreRef.content.asFb2();
      if (fb2Provider && fb2Provider->getFb2()) {
        auto* fb2 = fb2Provider->getFb2();
        bool didImageWork = false;
        if (fb2->hasDeferredImages()) {
          const int reissued = fb2->retryDeferredImages(shouldAbort);
          if (reissued > 0) {
            LOG_INF(TAG, "[CACHE] re-streamed %d deferred FB2 image(s)", reissued);
            didImageWork = true;
          }
        }
        if (fb2->hasPendingImages()) {
          // v2.0.69: pass UI-only abort callback to JPEG decode.  The
          // full callback aborts on `free<15K` mid-decode, which fires
          // routinely after the arena allocation drops free heap below
          // that threshold — even though decode itself only allocates
          // tiny LittleFS write buffers from there on.  The result was
          // a successful decode pattern of "first attempt aborts at
          // arena alloc, second attempt completes," doubling time-to-
          // image and often making the user flip past the page before
          // the image appeared.  UI-only callback lets a started
          // decode finish unless the user explicitly preempts.
          const auto decodeAbort = asyncJobs_.abortCallbackUiOnly();
          const int decoded = fb2->decodePendingImages(decodeAbort);
          if (decoded > 0) {
            LOG_INF(TAG, "[CACHE] decoded %d pending FB2 image(s)", decoded);
            didImageWork = true;
          }
        }
        if (didImageWork) {
          didBackgroundWork = true;
          // Force a current-page repaint so newly decoded images replace the
          // placeholder on the visible page (if any).  Reuses the same
          // pending-refresh slot as the EPUB cache rewrite path.
          pendingBackgroundEpubRefresh_ = true;
          pendingBackgroundEpubRefreshSpine_ = request.position.currentSpineIndex;
          pendingBackgroundEpubRefreshPage_ = request.position.currentSectionPage < 0
                                                  ? 0
                                                  : request.position.currentSectionPage;
          LOG_INF(TAG, "[CACHE] scheduled repaint after FB2 image work spine=%d page=%d",
                  request.position.currentSpineIndex, request.position.currentSectionPage);
        }
      }
    }

    // v2.0.83: drain EPUB async-decode queue.  When the chapter parser was
    // running in quick mode (background cache build / TOC-jump indexing)
    // it deferred JPEG → BMP conversion via PendingImageDecode::enqueue
    // and returned a synthetic success with peek-only dimensions.  Now
    // that the page batch is committed, do the real decode work in the
    // background.  Each completed BMP flips the global refresh signal so
    // the foreground reader picks up the image on its next render pass.
    //
    // Yield between decodes so the abort callback can fire — each decode
    // is 3-10 s and we don't want a button-press to wait that long.
    if (type == ContentType::Epub && !(shouldAbort && shouldAbort())) {
      const size_t pendingBefore = snapix::pendingImage::pendingCount();
      if (pendingBefore > 0) {
        LOG_INF(TAG, "[CACHE] draining %u pending EPUB image decode(s)",
                static_cast<unsigned>(pendingBefore));
        size_t drained = 0;
        while (snapix::pendingImage::drainOne()) {
          ++drained;
          if (shouldAbort && shouldAbort()) break;
          vTaskDelay(1);
        }
        if (drained > 0) {
          didBackgroundWork = true;
          pendingBackgroundEpubRefresh_ = true;
          pendingBackgroundEpubRefreshSpine_ = request.position.currentSpineIndex;
          pendingBackgroundEpubRefreshPage_ = request.position.currentSectionPage < 0
                                                  ? 0
                                                  : request.position.currentSectionPage;
          LOG_INF(TAG, "[CACHE] drained %u image(s); scheduled repaint spine=%d page=%d",
                  static_cast<unsigned>(drained), request.position.currentSpineIndex,
                  request.position.currentSectionPage);
        }
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

    // v2.0.105 (architectural — persistent parser state machine):
    //
    // Pre-fix, the TOC worker constructed a fresh `EpubChapterParser` on
    // the stack each invocation.  When the worker returned (e.g. after
    // signalling `pendingTocFirstPageReady_` so the main thread could
    // surface page 0), the parser was destroyed.  A subsequent worker
    // invocation built a brand-new parser starting from byte 0 of the
    // chapter source — `canResume()` returned false, so `PageCache::extend`
    // took the COLD path, throwing away the partial cache and re-parsing
    // the entire chapter just to add 25 more pages.  Observed on the
    // user's 56 KB Calibre chapter: a 1-page first batch took 1.8 s,
    // then the second invocation paid 25 s on a full cold rebuild.
    //
    // The fix is the same pattern the foreground BG cache controller
    // already uses (see ReaderCacheController::createOrExtendCache at
    // line ~1003): persist the parser in `ReaderDocumentResources::State`
    // keyed by spine.  Reuse when the spine matches and `canResume()`
    // returns true; create new only on spine change or non-resumable
    // state.  All TOC worker / BG cache / page-fill jobs run on the
    // same ReaderAsync task so there's no concurrency concern — the
    // session lock just makes the contract explicit.
    auto session = cacheController_.resources().acquireWorker("toc-worker");
    if (!session) {
      LOG_ERR(TAG, "[ASYNC] TOC worker: could not acquire resources spine=%d", request.targetSpine);
      return;
    }
    auto& state = session.state();

    EpubChapterParser* parser = nullptr;
    if (state.parser && state.parserSpineIndex == request.targetSpine &&
        state.parser->canResume()) {
      // Reuse the existing parser — its byte cursor + XML state still
      // point at the next unread chunk of the chapter source.  Saves
      // the full cold-rebuild on every TOC worker re-entry.
      parser = static_cast<EpubChapterParser*>(state.parser.get());
    } else {
      state.parser.reset();
      auto* epubParser = new EpubChapterParser(epub, request.targetSpine, renderer_, config, imageCachePath, true);
      // v2.0.131 — plumb real viewport margins so R4.c .idx build uses
      // the same paginator config as the render path.
      epubParser->setStreamingViewport(vp.marginTop, vp.marginBottom, vp.marginLeft, vp.marginRight);
      state.parser = std::unique_ptr<ContentParser>(epubParser);
      state.parserSpineIndex = request.targetSpine;
      parser = epubParser;
    }

    PageCache cache(cachePath);
    bool cacheLoaded = cache.load(config);
    if (cacheLoaded && !LittleFS.exists((cachePath + ".anchors").c_str())) {
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

    // v2.0.85: tiny first batch on fresh-spine TOC jumps so we can surface
    // page 0 ASAP via the deferred-display path.
    // v2.0.102: follow-up batches grow to 25 pages to amortize per-cycle
    // overhead.  shouldAbort is checked per-line so user preempts within
    // ~100 ms regardless of batch size.
    // v2.0.105 (this commit): parser state persists across worker
    // invocations.  After the first 1-page batch returns and the main
    // thread fires deferred-display, the NEXT TOC worker invocation
    // reuses the existing parser and calls `cache.extend(parser, 25)` —
    // hot path with canResume=1, ~5 s instead of 25 s for a fresh
    // 25-page extend on a heavy chapter.
    // v2.0.110 (audit fix #3): TOC worker has a strict batch budget after
    // deferred-display fires.  Pre-fix, the worker kept looping through
    // 25-page batches searching for the anchor — but once page 0 has been
    // surfaced to the user via deferred-display, finding the anchor isn't
    // user-critical anymore.  If the chapter is large and the anchor is
    // deep, the worker would grind for tens of seconds at priority=1,
    // starving `loopTask` and making buttons unresponsive (Agent #4 root
    // cause of "buttons stopped responding after first page shown").
    //
    // New rule: after the first batch surfaces page 0, the worker gets
    // ONE more follow-up batch budget.  If the anchor isn't found by
    // then, exit cleanly — the user has the page they need.  BG cache
    // controller will resume extending in its own time at priority=0.
    bool firstBatch = !cacheLoaded;
    bool deferredDisplayFired = false;
    uint8_t followUpBatchBudget = kEpubTocJumpFollowUpBatchBudget;
    while (!(shouldAbort && shouldAbort())) {
      const uint16_t batchSize = firstBatch ? kEpubTocJumpFirstBatchPages : kEpubTocJumpFollowUpBatchPages;
      const uint16_t pagesBeforeBatch = cache.pageCount();
      const bool success = cacheLoaded ? cache.extend(*parser, batchSize, shouldAbort)
                                       : cache.create(*parser, config, batchSize, 0, shouldAbort);
      if (!success) {
        LOG_ERR(TAG, "[ASYNC] TOC worker failed spine=%d", request.targetSpine);
        break;
      }

      // v2.0.104: signal main thread that the first page is on disk and the
      // cache file is closed.  Main thread's processPendingTocJump picks
      // this up on its next tick and does a one-shot reloadCacheFromDisk
      // followed by the deferred-display upgrade.
      if (firstBatch && cache.pageCount() > 0) {
        asyncJobs_.setPendingTocFirstPageReady(true);
        deferredDisplayFired = true;

        // v2.0.110 (audit fix #2): once page 0 is on disk and the main
        // thread can render it, this worker's remaining work — anchor
        // search through the rest of the chapter — is no longer
        // user-critical.  Drop our priority to 0 so `loopTask` (also
        // priority 1) gets preferential scheduling.  Audit Agent #6
        // confirmed equal-priority round-robin starves the UI for the
        // duration of long entropy decode / hyphenation passes; this
        // makes button input responsive within ~1 ms instead of waiting
        // for the parser's coarse ~100-iteration `vTaskDelay(1)`.
        vTaskPrioritySet(nullptr, 0);
        LOG_INF(TAG, "[ASYNC] TOC worker dropped to priority=0 after deferred-display spine=%d",
                request.targetSpine);
      }

      saveAnchorMap(*parser, cachePath);
      if (anchorResolved() || !cache.isPartial()) {
        break;
      }

      // v2.0.106: zero-progress circuit breaker.  When the parser hits a
      // permanent heap watermark (e.g. largest_free pinned at 7668 by
      // accumulated mid-heap fragmentation from a prior FB2 image decode
      // via JPEGDEC fallback), makePages aborts every layout attempt and
      // produces zero pages.  Without this guard, the outer while-loop
      // re-invokes the parser on the SAME unrecoverable state forever
      // (observed in the user's spine=12 trace: ~50 STOP_TRACE retries
      // over ~6 s with no page growth).  Break out cleanly so the
      // outer pendingTocJump retry mechanism (capped at
      // `kPendingTocJumpMaxRetries`) can decide whether to give up or
      // wait for heap recovery.
      if (cache.pageCount() == pagesBeforeBatch) {
        LOG_INF(TAG, "[ASYNC] TOC worker no-progress spine=%d batch=%u (parser aborted, heap likely fragmented)",
                request.targetSpine, static_cast<unsigned>(batchSize));
        break;
      }

      cacheLoaded = true;
      firstBatch = false;

      // v2.0.110 (audit fix #3): post-deferred-display batch budget.
      // After page 0 is on disk and surfaced to the user, give the
      // anchor search ONE more follow-up batch of work (25 more pages),
      // then exit even if anchor still not found.  The user's
      // background-cache controller will resume extending at priority=0
      // — anchor will be discovered on its own timeline, or the user
      // can re-issue the TOC jump if they actually need it.  This is
      // the "stop being aggressive after the user already has their
      // content" rule that the audit identified as the kill condition.
      if (deferredDisplayFired) {
        if (followUpBatchBudget == 0) {
          LOG_INF(TAG,
                  "[ASYNC] TOC worker exit after post-deferred-display budget spine=%d "
                  "(page 0 shown, anchor not yet found — BG cache will continue)",
                  request.targetSpine);
          break;
        }
        --followUpBatchBudget;
      }
      vTaskDelay(1 / portTICK_PERIOD_MS);
    }
    return;
  }

  const std::string cachePath = contentCachePath(coreRef.content.cacheDir(), config.fontId);
  auto* provider = coreRef.content.asFb2();
  auto* fb2 = provider ? provider->getFb2() : nullptr;
  const uint32_t targetSourceOffset = resolveFb2AnchorSourceOffset(fb2, targetAnchor);
  Fb2Parser parser(contentPath_, renderer_, config);
  if (fb2) parser.setFb2(fb2);
  PageCache cache(cachePath);
  bool cacheLoaded = cache.load(config);
  int targetPageHint = request.targetPageHint;
  if (cacheLoaded && !LittleFS.exists((cachePath + ".anchors").c_str())) {
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
        auto* epubParser = new EpubChapterParser(provider->getEpubShared(), request.targetSpine, renderer_, config,
                                                 imageCachePath, true);
        // v2.0.131 — plumb real viewport margins (see background-cache site).
        epubParser->setStreamingViewport(vp.marginTop, vp.marginBottom, vp.marginLeft, vp.marginRight);
        parser_.reset(epubParser);
        parserSpineIndex_ = request.targetSpine;
      } else if (type == ContentType::Fb2) {
        auto* fb2Provider = coreRef.content.asFb2();
        uint32_t startOffset = 0;
        uint32_t endOffset = 0;
        int startingSectionIndex = 0;
        if (resolveFb2SectionContext(fb2Provider, config, request.targetSpine, nullptr, &startOffset,
                                     &startingSectionIndex, &endOffset)) {
          auto* fb2Parser = new Fb2Parser(contentPath_, renderer_, config, startOffset, startingSectionIndex, true, endOffset);
          if (fb2Provider && fb2Provider->getFb2()) fb2Parser->setFb2(fb2Provider->getFb2());
          // v2.0.131 — plumb real viewport margins (see background-cache site).
          fb2Parser->setStreamingViewport(vp.marginTop, vp.marginBottom, vp.marginLeft, vp.marginRight);
          parser_.reset(fb2Parser);
          parserSpineIndex_ = request.targetSpine;
        } else {
          auto* fb2Parser = new Fb2Parser(contentPath_, renderer_, config);
          if (fb2Provider && fb2Provider->getFb2()) fb2Parser->setFb2(fb2Provider->getFb2());
          // v2.0.131 — plumb real viewport margins (see background-cache site).
          fb2Parser->setStreamingViewport(vp.marginTop, vp.marginBottom, vp.marginLeft, vp.marginRight);
          parser_.reset(fb2Parser);
          parserSpineIndex_ = 0;
        }
      } else if (type == ContentType::Markdown) {
        // v2.0.192 — route through Markdown's effective content path;
        // see matching comment in the background-cache path above.
        const auto* mdProv = coreRef.content.asMarkdown();
        const Markdown* md = mdProv ? mdProv->getMarkdown() : nullptr;
        const std::string parserPath = md ? md->getEffectiveContentPath() : std::string(contentPath_);
        const bool useLfs = md ? md->isContentOnLittleFs() : false;
        parser_.reset(new MarkdownParser(parserPath, renderer_, config, useLfs));
        parserSpineIndex_ = 0;
      } else if (type == ContentType::Html) {
        // v2.0.162 — HtmlParser deleted; see comment in the analogous
        // branch in scheduleBackgroundCacheJob above.
      } else if (type == ContentType::Txt) {
        // v2.0.189 — route through Txt's effective content path; see
        // matching comment in the background-cache path above.
        const auto* txtProv = coreRef.content.asTxt();
        const Txt* txt = txtProv ? txtProv->getTxt() : nullptr;
        const std::string parserPath = txt ? txt->getEffectiveContentPath() : std::string(contentPath_);
        const bool useLfs = txt ? txt->isContentOnLittleFs() : false;
        parser_.reset(new PlainTextParser(parserPath, renderer_, config, useLfs));
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
        const reader::HeapState heap = reader::readHeapState();
        const int speculativeHeadroom = reader::isHeapTight(heap)
                                            ? 0
                                            : static_cast<int>(reader::kEpubInteractivePageFillHeadroomPages);
        const int desiredPages = std::max(1, remainingPages + speculativeHeadroom);
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

}  // namespace snapix
