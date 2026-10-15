#pragma once

#include <Epub.h>
#include <RenderConfig.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ContentParser.h"
#include "StreamingSection.h"  // ChunkedIdxState / ChunkedIdxExtend (v3.11.12 progressive chapters)

class GfxRenderer;
class ZipItemReader;
namespace snapix::unifiedcache {
class UnifiedCache;
}
namespace snapix::smolport {
class ChunkedMarkerizer;
class CssStyleTable;
}

/**
 * Content parser for EPUB chapters.
 *
 * v2.0.161 — only drives the v3 streaming pipeline:
 *   tryMarkerizeChapter (ZIP → HtmlStripper → markers/N.bin)
 *   → R4.c upfront idx build
 *   → R4.b short-circuit emits empty Page objects from idx
 *
 * Streaming render in `ReaderState::renderPageContents` then reads markers
 * directly for layout + drawing.  The legacy `ChapterHtmlSlimParser` parse
 * loop, `prepareChapterHtml` extract-to-temp-file, and the resume path
 * for the suspended expat parser are all GONE — the R-series of fixes
 * (R2-R5) replaced every use of them in v3_alpha.
 *
 * Default env (SNAPIX_MARKERIZER=0): the markerize blocks compile out
 * to a no-op stub.  An EPUB book in default env now returns empty
 * pages — the legacy fallback is removed.  v3_alpha is the supported
 * configuration; default env is a build-test only.
 */
class EpubChapterParser : public ContentParser {
  std::shared_ptr<Epub> epub_;
  int spineIndex_;
  GfxRenderer& renderer_;
  RenderConfig config_;
  std::string imageCachePath_;
  bool hasMore_ = true;

  std::string chapterBasePath_;
  bool initialized_ = false;

  // Callback state shared between init and resume paths.
  // The liveParser_'s completePageFn captures `this` and delegates to these members,
  // so the callback can be rewired between parsePages() calls without recreating the parser.
  std::function<void(std::unique_ptr<Page>)> onPageComplete_;
  uint16_t maxPages_ = 0;
  uint16_t pagesCreated_ = 0;
  bool hitMaxPages_ = false;

  // Streaming anchor map populated during the same MEASURE walk that builds
  // the page index.  v2.0.186 removed the old legacy-parser map, but the
  // replacement streaming path never wired anchors back in; every cold TOC
  // jump therefore remained unresolved and fell back to page 0.
  std::vector<std::pair<std::string, uint16_t>> anchorMap_;

  // v2.0.118 Phase R2.8 — markerize one-shot guard.  Set on first
  // tryMarkerizeChapter call regardless of outcome (success, write
  // error, etc.) so we don't re-attempt within the same parser
  // instance.  Cross-instance retry happens naturally because the
  // file existence check fires first; the new instance will skip
  // if `markers/<spine>.bin` is already on disk.
  bool markerizeAttempted_ = false;

  // v2.0.131 — real viewport margins from ReaderState.  R4.c (upfront
  // .idx build via MEASURE walk) needs the SAME paginator config the
  // render path uses, else the configHash differs and ReaderState
  // discards the parser-built .idx on first render → wasteful rebuild
  // AND pageCount mismatch (parser-with-4px-margins says N pages,
  // renderer-with-real-margins produces ≠N pages).  PageCount mismatch
  // caused the v2.0.130 "FB2 white screen on page turn" — empty Page
  // objects emitted by R4.b based on parser's wrong count fell through
  // to legacy page.render() on the over-counted pages.
  //
  // Defaults of 4 preserve v2.0.130 behaviour when setter never called
  // (e.g. legacy code paths that don't have a viewport yet).  Async
  // jobs always set them right after construction.
  int streamingViewportMarginTop_ = 4;
  int streamingViewportMarginBottom_ = 4;
  int streamingViewportMarginLeft_ = 4;
  int streamingViewportMarginRight_ = 4;

  // v2.0.132 — stateful R4.b short-circuit progress tracking.  Before
  // this fix, R4.b emitted up to maxPages empty Pages and set
  // hasMore_=false unconditionally.  Cache called with maxPages=1
  // (typical first-batch sizing for snappy first-page surfacing) wrote
  // a single-page complete cache file, claiming the whole chapter was
  // 1 page even though .idx said 33.  User couldn't navigate past
  // page 0, OR the cache extend re-ran the short-circuit from scratch
  // each invocation (same first N pages forever, never reaching tail).
  //
  // Now: first activation reads totalPages from .idx, emits up to
  // maxPages of those, sets `shortCircuitActive_=true` and
  // hasMore_ = (emitted < total).  Subsequent parsePages calls find
  // shortCircuitActive_=true at the top, continue emitting from
  // shortCircuitNextPage_, advance the counter, update hasMore_.
  // When emitted == total, set hasMore_=false and shortCircuitActive_
  // =false so the cache marks the file complete.
  //
  // `canResume()` returns true while shortCircuitActive_ so the cache
  // controller treats us as a resumable parser (avoids cold-rebuild
  // path on subsequent extends).
  bool shortCircuitActive_ = false;
  uint16_t shortCircuitTotalPages_ = 0;
  uint16_t shortCircuitNextPage_ = 0;

  // Cold TOC jumps ask for a one-page first batch.  Publish that page after
  // markerization, before the expensive whole-chapter MEASURE walk, then
  // resume the same parser in the background to build the complete idx.
  bool bootstrapPageEmitted_ = false;
  bool awaitingIndexBuild_ = false;

  // v2.0.116 Phase R2 — markerize side-channel, now (v2.0.159) the
  // ONLY chapter ingestion path.  Streams chapter source straight from
  // the ZIP archive through HtmlStripper into `markers/N.bin`.  Returns
  // true on success or cache hit; false on ZIP/uzlib/I/O failure.
  // Gated by `SNAPIX_MARKERIZER`; default env compiles to an empty stub
  // that returns true silently.
  //
  // v2.0.118: deliberately does NOT take a shouldAbort callback.  User
  // input shouldn't interrupt the side-channel side of the markerize
  // pass — otherwise the sidecar never gets written for the user's
  // most-pressured chapters.  markerizeAttempted_ guards in-instance
  // retry; cross-instance retry happens via the file-existence check.
  bool tryMarkerizeChapter(snapix::unifiedcache::UnifiedCache& cache);

  // ---- v3.11.12 — progressive (chunked) markerize for BIG chapters ---------
  // A chapter whose source exceeds kProgressiveMinSourceBytes is markerized in
  // 32 KB source chunks (UnifiedCache markerChunkKind(k), key = spine) instead
  // of one Markers frame, and its idx grows chunk by chunk, so pages are
  // emitted as soon as the walk has passed them — a saved page deep in a
  // single-file book no longer waits for the whole chapter (60+ s on device).
  // One HtmlStripper stays alive across the chunks (markers byte-identical to
  // one pass); the ZIP stream is reopened per call and skipped forward, so its
  // 32 KB inflate window is never held between calls.  Small chapters keep the
  // one-shot tryMarkerizeChapter + R4.c path below.
  bool progressiveDecided_ = false;
  bool progressive_ = false;
  bool progInit_ = false;
  uint32_t progSrcSize_ = 0;  // uncompressed chapter bytes (valid when known)
  bool progSrcSizeKnown_ = false;
  int progChunks_ = 0;            // chunk frames stored
  bool progSourceDone_ = false;   // all chunks stored (Kind::MarkerChunksDone)
  bool progIdxComplete_ = false;  // idx walk reached the end of the last chunk
  int progIdxChunks_ = 0;         // chunks covered by a walk since init
  snapix::pagecache::ChunkedIdxState progIdx_;
  uint16_t progEmitCursor_ = 0;
  // Live stripper positioned where the stored chunks end.  Survives reset()
  // while clean: rebuilding one replays (inflates + strips) the whole prefix.
  std::unique_ptr<snapix::smolport::ChunkedMarkerizer> progMarkerizer_;
  // Opened by the size decision; consumed (from byte 0) by whichever path
  // markerizes in the same parsePages call, and never kept past it.
  std::unique_ptr<ZipItemReader> pendingStream_;

  // ---- v3.11.12 — the book's CSS (class / inline-style emphasis) -----------
  // Compiled once per book from its stylesheets into UnifiedCache
  // Kind::StyleTable and loaded before a chapter markerizes; the stripper
  // resolves every element against it.  Kept while progMarkerizer_ lives (its
  // stripper points at it) so every chunk of a chapter sees identical rules.
  std::unique_ptr<snapix::smolport::CssStyleTable> styleTable_;
  bool styleTableReady_ = false;  // styleTable_ holds the cached table
  bool ensureStyleTable(snapix::unifiedcache::UnifiedCache& cache);

  bool useProgressiveMarkers(snapix::unifiedcache::UnifiedCache& cache);
  bool ensureProgressiveInit(snapix::unifiedcache::UnifiedCache& cache);
  void rebuildProgressiveAnchors(snapix::unifiedcache::UnifiedCache& cache);
  int markerizeProgressiveChunks(snapix::unifiedcache::UnifiedCache& cache, int wanted,
                                 const AbortCallback& shouldAbort);
  snapix::pagecache::ChunkedIdxExtend extendProgressiveIdx(snapix::unifiedcache::UnifiedCache& cache,
                                                           const AbortCallback& shouldAbort);
  bool parsePagesProgressive(snapix::unifiedcache::UnifiedCache& cache,
                             const std::function<void(std::unique_ptr<Page>)>& onPageComplete,
                             uint16_t maxPages, const AbortCallback& shouldAbort);
  uint16_t progPagesAvailable() const {
    return static_cast<uint16_t>(progIdx_.boundaries.size() + (progIdxComplete_ ? 1 : 0));
  }
  uint16_t computeStreamingConfigHash() const;

 public:
  EpubChapterParser(std::shared_ptr<Epub> epub, int spineIndex, GfxRenderer& renderer, const RenderConfig& config,
                    const std::string& imageCachePath = "");
  ~EpubChapterParser() override;

  bool parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages = 0,
                  const AbortCallback& shouldAbort = nullptr) override;
  bool hasMoreContent() const override { return hasMore_; }
  bool canResume() const override;
  void reset() override;
  const std::vector<std::pair<std::string, uint16_t>>& getAnchorMap() const override {
    return anchorMap_;
  }

  // Compatibility for caches produced by v3.11.2 and older: map anchor marker
  // offsets onto an existing current page index without rerunning pagination.
  bool rebuildAnchorMapFromCachedIndex();

  // v2.0.131 — caller (ReaderStateAsyncJobs) plumbs the actual reader
  // viewport margins here right after construction so the R4.c upfront
  // .idx build uses the SAME paginator config that the streaming
  // render path (ReaderState::renderPageContents) will use at render
  // time.  Without this, parser hashed its idx with 4-px defaults and
  // the render path discarded it as "stale/corrupt" on every first
  // visit — wasting the entire MEASURE walk.  Worse, the parser's
  // pageCount with 4-px margins ≠ renderer's pageCount with real
  // margins, so R4.b emitted the wrong number of empty Pages, causing
  // white-screen on page turn past the renderable end.
  void setStreamingViewport(int marginTop, int marginBottom, int marginLeft, int marginRight) {
    streamingViewportMarginTop_ = marginTop;
    streamingViewportMarginBottom_ = marginBottom;
    streamingViewportMarginLeft_ = marginLeft;
    streamingViewportMarginRight_ = marginRight;
  }
};
