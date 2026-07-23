#include "EpubChapterParser.h"

#include <Arduino.h>
#include <FS.h>          // Arduino base File (LittleFS, v2.0.73)
#include <FsHelpers.h>
#include <LittleFS.h>
// v2.0.161 — dropped `<Epub/parsers/ChapterHtmlSlimParser.h>` and
// `<Html5Normalizer.h>` — both belonged to the legacy expat parse path
// that no longer runs from this translation unit.  ChapterHtmlSlimParser
// still exists for `cacheImageForStreaming` (used by ReaderState) and
// for the standalone HtmlParser; we just don't pull it in here.
#include <GfxRenderer.h>
#include <Hyphenation.h>
#include <Logging.h>
#include <Page.h>
#include <SDCardManager.h>
#include <UnifiedCache.h>  // v2.0.167 — Phase 5 unified streaming cache
#include <ZipFile.h>  // v2.0.159 — ZipItemReader for streaming markerize-from-ZIP
#include <core/CrashDebug.h>
#include <esp_heap_caps.h>

// v2.0.116 Phase R2 — markerize side-channel.  Active only when v3_alpha
// (or any env) defines SNAPIX_MARKERIZER=1.  Default env compiles the
// `tryMarkerizeChapter` stub to an empty function and the SmolPort
// headers below are excluded from the include set entirely.
//
// v2.0.130 R4.c — adds the streaming paginator + adapter so the parser
// can build the `.idx` upfront via a MEASURE-only walk, eliminating
// the legacy parser dependency even on a chapter's first visit.
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
#include <GfxRendererPaginatorAdapter.h>
#include <HtmlStripper.h>
#include <MarkerizedPageRender.h>
#include <MarkerizeChapter.h>
#include <MarkerStream.h>
#include <StreamingPaginator.h>
#endif

#define TAG "EPUB_CHAP"

#include <algorithm>
#include <memory>   // unique_ptr — heap the MEASURE-walk buffers off the 20 KB task stack
#include <new>      // std::nothrow
#include <utility>

namespace {

// v2.0.116 Phase R2 — marker sidecar lives at `<cachePath>/markers/<spine>.bin`,
// parallel to `<cachePath>/sections/`.  The directory is auto-created via
// `ensureCacheDirRecursive` (mirrors the PageCache helper but kept inline
// here so EpubChapterParser stays self-contained).
//
// v2.0.161 — the `<cachePath>/chapters/` directory is gone too.  It used
// to hold `N.src.html` / `N.norm.html` temp files for the legacy parser's
// extract+normalize phases.  Both phases are now dead code; the markerize-
// from-ZIP fast path streams straight from the archive into markers/.
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
inline std::string epubMarkersDir(const std::string& epubCachePath) {
  return epubCachePath + "/markers";
}
inline std::string epubMarkersPath(const std::string& epubCachePath, int spineIndex) {
  return epubMarkersDir(epubCachePath) + "/" + std::to_string(spineIndex) + ".bin";
}
bool ensureCacheDirRecursive(const std::string& path) {
  if (path.empty() || path == "/") return true;
  if (LittleFS.exists(path.c_str())) return true;
  size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos && lastSlash > 0) {
    if (!ensureCacheDirRecursive(path.substr(0, lastSlash))) return false;
  }
  return LittleFS.mkdir(path.c_str());
}

snapix::smolport::StreamingPaginatorConfig makeEpubStreamingConfig(
    GfxRenderer& renderer, const RenderConfig& config, int marginTop,
    int marginBottom, int marginLeft, int marginRight,
    const std::string& language) {
  const uint16_t bodyLineH =
      static_cast<uint16_t>(renderer.getLineHeight(config.fontId) *
                            config.lineCompression);
  snapix::smolport::StreamingPaginatorConfig cfg{};
  cfg.pageWidth = static_cast<uint16_t>(renderer.getScreenWidth());
  cfg.pageHeight = static_cast<uint16_t>(renderer.getScreenHeight());
  cfg.marginTop = static_cast<uint16_t>(marginTop);
  cfg.marginBottom = static_cast<uint16_t>(marginBottom);
  cfg.marginLeft = static_cast<uint16_t>(marginLeft);
  cfg.marginRight = static_cast<uint16_t>(marginRight);
  cfg.bodyLineHeight = bodyLineH > 0 ? bodyLineH : 24;
  cfg.headingLineHeight = static_cast<uint16_t>(cfg.bodyLineHeight * 3 / 2);
  cfg.paragraphSpacing = snapix::smolport::paragraphSpacingForLevel(
      config.spacingLevel, cfg.bodyLineHeight);
  cfg.firstLineIndent = snapix::smolport::firstLineIndentForLevel(
      config.indentLevel, cfg.bodyLineHeight);
  cfg.hyphenate = true;
  std::strncpy(cfg.hyphenLang, language.c_str(), sizeof(cfg.hyphenLang) - 1);
  cfg.hyphenLang[sizeof(cfg.hyphenLang) - 1] = '\0';
  return cfg;
}

class IndexedAnchorObserver final : public snapix::smolport::MarkerObserver {
 public:
  IndexedAnchorObserver(
      const std::vector<snapix::smolport::PageBoundarySnapshot>& boundaries,
      std::vector<std::pair<std::string, uint16_t>>& anchors)
      : boundaries_(boundaries), anchors_(anchors) {}

  snapix::smolport::ObserverStatus onAnchorAt(
      const uint8_t* id, uint16_t len, uint32_t sourceOffset) override {
    while (nextBoundary_ < boundaries_.size() &&
           boundaries_[nextBoundary_].byteOffset <= sourceOffset) {
      currentPage_ = boundaries_[nextBoundary_].pageIndex;
      ++nextBoundary_;
    }
    if (id != nullptr && len > 0) {
      anchors_.emplace_back(
          std::string(reinterpret_cast<const char*>(id), len), currentPage_);
    }
    return snapix::smolport::ObserverStatus::Continue;
  }

 private:
  const std::vector<snapix::smolport::PageBoundarySnapshot>& boundaries_;
  std::vector<std::pair<std::string, uint16_t>>& anchors_;
  size_t nextBoundary_ = 0;
  uint16_t currentPage_ = 0;
};
#endif

}  // namespace

EpubChapterParser::EpubChapterParser(std::shared_ptr<Epub> epub, int spineIndex, GfxRenderer& renderer,
                                     const RenderConfig& config,
                                     const std::string& imageCachePath)
    : epub_(std::move(epub)),
      spineIndex_(spineIndex),
      renderer_(renderer),
      config_(config),
      imageCachePath_(imageCachePath) {}

EpubChapterParser::~EpubChapterParser() = default;

void EpubChapterParser::reset() {
  initialized_ = false;
  hasMore_ = true;
  chapterBasePath_.clear();
  anchorMap_.clear();
  // v2.0.132 — drop any in-flight short-circuit state too.
  shortCircuitActive_ = false;
  shortCircuitTotalPages_ = 0;
  shortCircuitNextPage_ = 0;
  bootstrapPageEmitted_ = false;
  awaitingIndexBuild_ = false;
  // v2.0.161 — also reset markerize-once guard so a fresh parser
  // instance re-evaluates the markers cache (file-existence check
  // inside tryMarkerizeChapter still short-circuits on disk hit).
  markerizeAttempted_ = false;
}

bool EpubChapterParser::canResume() const {
  // v2.0.161 — only the R4.b stateful short-circuit resumes.  The legacy
  // liveParser_ resume path is gone (parser deleted with the rest of the
  // extract pipeline); the cache controller treats canResume()==true as
  // "hot-extend OK, no need to cold-rebuild", which is exactly what the
  // mid-emit short-circuit wants.
  return awaitingIndexBuild_ ||
         (shortCircuitActive_ && shortCircuitNextPage_ < shortCircuitTotalPages_);
}

bool EpubChapterParser::rebuildAnchorMapFromCachedIndex() {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  auto cache =
      snapix::unifiedcache::UnifiedCache::shared(epub_->getCachePath());

  std::vector<uint8_t> idxPayload;
  if (!cache.readSegment(snapix::unifiedcache::Kind::Idx,
                         static_cast<uint16_t>(spineIndex_), idxPayload)) {
    return false;
  }

  const auto cfg = makeEpubStreamingConfig(
      renderer_, config_, streamingViewportMarginTop_,
      streamingViewportMarginBottom_, streamingViewportMarginLeft_,
      streamingViewportMarginRight_, epub_->getLanguage());
  const uint16_t configHash = snapix::smolport::computePageIndexConfigHash(
      cfg, config_.fontId, config_.fakeBold);

  std::vector<snapix::smolport::PageBoundarySnapshot> boundaries;
  if (!snapix::smolport::deserializePageIndex(
          idxPayload.data(), idxPayload.size(), configHash, boundaries)) {
    return false;
  }

  File markerFile;
  size_t markerBytes = 0;
  if (!cache.openSegmentReader(snapix::unifiedcache::Kind::Markers,
                               static_cast<uint16_t>(spineIndex_), markerFile,
                               &markerBytes) ||
      markerBytes == 0) {
    return false;
  }

  anchorMap_.clear();
  IndexedAnchorObserver observer(boundaries, anchorMap_);
  snapix::smolport::MarkerStreamReader reader(observer);
  constexpr size_t kScanBufferBytes = 4096;
  std::unique_ptr<uint8_t[]> scanBuffer(
      new (std::nothrow) uint8_t[kScanBufferBytes]);
  if (!scanBuffer) {
    markerFile.close();
    return false;
  }

  size_t remaining = markerBytes;
  bool ok = true;
  while (remaining > 0) {
    const size_t want = std::min(remaining, kScanBufferBytes);
    const int got = markerFile.read(scanBuffer.get(), want);
    if (got <= 0 ||
        !reader.feed(scanBuffer.get(), static_cast<size_t>(got))) {
      ok = false;
      break;
    }
    remaining -= static_cast<size_t>(got);
  }
  if (ok) ok = reader.finish();
  markerFile.close();

  if (!ok) {
    anchorMap_.clear();
    return false;
  }
  LOG_INF(TAG,
          "[CONTENT][EPUB] anchor map rebuilt from idx spine=%d anchors=%u",
          spineIndex_, static_cast<unsigned>(anchorMap_.size()));
  return !anchorMap_.empty();
#else
  return false;
#endif
}

// =============================================================================
// v2.0.161 — `prepareChapterHtml` is deleted.  See file-scope changelog for
// rationale; in short, the legacy ChapterHtmlSlimParser parse loop that
// consumed the extracted temp file is no longer reachable, so the extract
// phase has no consumer.
// =============================================================================
// =============================================================================
// v2.0.116 Phase R2 — markerize the prepared chapter HTML to a SmolPort
// byte-marker sidecar.  Best-effort, side-channel: failures log but don't
// fail the parser.  Compiled out entirely in default env.
//
// v2.0.118 Phase R2.8 — does NOT take shouldAbort.  Pre-fix passed the
// same abort callback the legacy parser uses (cancel-on-button-press),
// which meant a button press while preparing chapter HTML killed the
// markerize pass at byte 0 → sidecar never produced for the chapters
// users actually navigated to.  The markerize side-channel takes
// ~200-500 ms per cold extend in v3_alpha; running it uninterrupted
// is preferable to never producing the sidecar.
// =============================================================================
bool EpubChapterParser::tryMarkerizeChapter(snapix::unifiedcache::UnifiedCache& cache) {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  if (markerizeAttempted_) return true;
  markerizeAttempted_ = true;

  // v2.0.167 — markers now live as a UnifiedCache::Markers segment in
  // <bookCachePath>/streaming.cache instead of per-spine <markers>/<N>.bin
  // files.  Cache-hit check: ask UnifiedCache for the segment size.
  size_t existingSize = 0;
  if (cache.segmentSize(snapix::unifiedcache::Kind::Markers,
                         static_cast<uint16_t>(spineIndex_), &existingSize)) {
    LOG_INF(TAG, "[CONTENT][EPUB] markerize cache hit spine=%d (UnifiedCache::Markers size=%zu)",
            spineIndex_, existingSize);
    return true;
  }

  // v2.0.159 — stream the chapter source directly from the ZIP archive
  // instead of reading from `parseHtmlPath_` (which used to require a
  // prior `prepareChapterHtml` extract to a LittleFS temp file).
  //
  // Heap budget during this fast-path markerize:
  //   * 32 KB uzlib dict (inside InflateReader, freed when zipStream dies)
  //   * 4 KB SD read buffer (inside ZipItemReader, freed in dtor)
  //   * 4 KB markerize chunk buffer (stack-resident)
  //   * ~600 B HtmlStripper state (stack-resident inside markerizeChapter)
  // Total ~41 KB transient, all freed before this method returns.  The
  // old path needed ~53 KB transient for extract (32 KB dict + 16 KB
  // I/O + state) PLUS a second pass over the just-written temp file for
  // markerize — so this is both lower-peak AND single-pass.
  //
  // Eliminates the prepare-extract phase entirely on this code path: no
  // temp `chapters/N.src.html` file gets written for any chapter whose
  // markers fit through HtmlStripper cleanly (i.e., the common case in
  // v3_alpha).  `prepareChapterHtml` is only called downstream IF the
  // R4.b short-circuit fails and the legacy parser actually has to run.
  const auto spineItem = epub_->getSpineItem(spineIndex_);
  if (spineItem.href.empty()) {
    LOG_ERR(TAG, "[CONTENT][EPUB] markerize no href spine=%d", spineIndex_);
    return false;
  }
  // dictBuffer = nullptr → InflateReader heap-allocates its own 32 KB.
  // Won't race with framebuffer (v2.0.155 lesson) and we don't pay a
  // permanent BSS reservation (v2.0.156 lesson).
  const size_t heapBeforeZip =
      heap_caps_get_free_size(MALLOC_CAP_8BIT);
  // The hardware profile showed a 4.9 KB historical minimum while opening a
  // cold EPUB chapter.  Match the compressed-input buffer to the 4 KB
  // markerizer chunk and recover 4 KB of peak headroom; the modest increase
  // in ZIP refill calls is preferable to operating within 5 KB of OOM.
  auto zipStream =
      epub_->openItemStream(spineItem.href, 4096, /*dictBuffer=*/nullptr);
  if (!zipStream) {
    LOG_ERR(TAG, "[CONTENT][EPUB] markerize open ZIP stream failed spine=%d href=%s", spineIndex_,
            spineItem.href.c_str());
    return false;
  }
  const size_t heapWithZip =
      heap_caps_get_free_size(MALLOC_CAP_8BIT);

  // v2.0.167 — write markers as a deferred-streaming UnifiedCache segment.
  // The markerize pass discovers payload size only after HtmlStripper finishes,
  // so we use writeSegmentStreamingDeferred which patches the size field
  // after the write callback returns.  No more `.work` temp file + rename.
  constexpr size_t kChunkBufBytes = 4096;
  uint8_t chunkBuf[kChunkBufBytes];

  snapix::smolport::MarkerizeStats stats{};
  snapix::smolport::MarkerizeStatus status = snapix::smolport::MarkerizeStatus::ReadError;
  const bool ok = cache.writeSegmentStreamingDeferred(
      snapix::unifiedcache::Kind::Markers, static_cast<uint16_t>(spineIndex_),
      [&](File& outFile) -> bool {
        auto readFn = [&zipStream](uint8_t* buf, size_t bufSize) -> int {
          if (!zipStream) return -1;
          return zipStream->read(buf, bufSize);
        };
        auto writeFn = [&outFile](const uint8_t* data, size_t len) -> bool {
          if (!outFile) return false;
          const size_t n = outFile.write(data, len);
          return n == len;
        };
        status = snapix::smolport::markerizeChapter(
            snapix::smolport::HtmlStripper::Mode::Html, readFn, writeFn, chunkBuf, sizeof(chunkBuf),
            {}, &stats);
        return status == snapix::smolport::MarkerizeStatus::Success;
      });

  zipStream.reset();  // free uzlib dict + ZIP file handle
  const size_t heapAfterZip =
      heap_caps_get_free_size(MALLOC_CAP_8BIT);
  LOG_INF(TAG,
          "[PERF] markerize ZIP heap spine=%d before=%u active=%u after=%u",
          spineIndex_, static_cast<unsigned>(heapBeforeZip),
          static_cast<unsigned>(heapWithZip),
          static_cast<unsigned>(heapAfterZip));

  if (!ok || status != snapix::smolport::MarkerizeStatus::Success) {
    LOG_ERR(TAG,
            "[CONTENT][EPUB] markerize failed spine=%d status=%u in=%u out=%u chunks=%u (UnifiedCache ok=%u)",
            spineIndex_, static_cast<unsigned>(status), static_cast<unsigned>(stats.inputBytes),
            static_cast<unsigned>(stats.outputBytes), static_cast<unsigned>(stats.chunksProcessed),
            static_cast<unsigned>(ok));
    return false;
  }

  LOG_INF(TAG,
          "[CONTENT][EPUB] markerize done spine=%d in=%u out=%u chunks=%u (UnifiedCache::Markers)",
          spineIndex_, static_cast<unsigned>(stats.inputBytes), static_cast<unsigned>(stats.outputBytes),
          static_cast<unsigned>(stats.chunksProcessed));
  return true;
#else
  return true;  // markerizer disabled in this build — silent success
#endif
}

bool EpubChapterParser::parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages,
                                   const AbortCallback& shouldAbort) {
  LOG_INF(TAG, "[CONTENT][EPUB] parsePages start spine=%d initialized=%u resumable=%u maxPages=%u", spineIndex_,
          static_cast<unsigned>(initialized_), static_cast<unsigned>(canResume()), static_cast<unsigned>(maxPages));

#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  // v2.0.132 — R4.b STATEFUL short-circuit continuation.  If a previous
  // parsePages call activated short-circuit but didn't finish emitting
  // all pages from .idx (because cache called us with maxPages < total),
  // pick up where we left off without re-opening .idx or re-running any
  // setup.  Cache's extend() loop hits us repeatedly with batches of
  // maxPages until hasMore_ goes false.
  if (shortCircuitActive_) {
    // Do not scan/extract chapter images here.  This branch is the critical
    // path for restoring a saved page, and image-heavy chapters can contain
    // hundreds of slow ZIP entries.  The foreground streaming renderer queues
    // only images it actually encounters; ReaderAsync decodes those after the
    // text page is visible.
    onPageComplete_ = onPageComplete;
    maxPages_ = maxPages;
    pagesCreated_ = 0;
    const uint16_t startPage = shortCircuitNextPage_;
    while (shortCircuitNextPage_ < shortCircuitTotalPages_) {
      if (maxPages > 0 && pagesCreated_ >= maxPages) break;
      onPageComplete(std::unique_ptr<Page>(new Page));
      pagesCreated_++;
      shortCircuitNextPage_++;
    }
    hasMore_ = (shortCircuitNextPage_ < shortCircuitTotalPages_);
    if (!hasMore_) {
      // Final batch — clean up so a future hot-extend re-evaluates
      // .idx (it'll find it complete and short-circuit-noop).
      shortCircuitActive_ = false;
      initialized_ = false;
      renderer_.clearWidthCache();
    }
    LOG_INF(TAG,
            "[CONTENT][EPUB] [STREAM] short-circuit batch spine=%d emitted=%u..%u of %u hasMore=%u",
            spineIndex_, static_cast<unsigned>(startPage),
            static_cast<unsigned>(shortCircuitNextPage_),
            static_cast<unsigned>(shortCircuitTotalPages_), static_cast<unsigned>(hasMore_));
    return pagesCreated_ > 0;
  }
#endif

  // v2.0.161 — legacy resume path deleted with the rest of the legacy
  // parser pipeline.  parsePages now has exactly one entry point: build
  // markers + idx, then short-circuit emit empty Pages from idx.  If a
  // future build needs a different (non-streaming) ingestion strategy,
  // it goes here.
  // INIT PATH: first call — markerize, build idx, short-circuit emit.
  // Hyphenation language is still set so HtmlStripper can hyphenate
  // long words during the markerize walk (HtmlStripper consults the
  // global hyphenation table indirectly via the layout adapter that
  // R4.c instantiates below).
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

  // v2.0.159 — try the FAST PATH first: stream the chapter source
  // directly from the ZIP archive, write markers, build idx, fire
  // R4.b short-circuit.  When this works (the common case in
  // v3_alpha), the slow `prepareChapterHtml` extract is never run
  // and the device avoids ~50 KB transient heap peak + ~10 s wall
  // time + a temp `chapters/N.src.html` file on LittleFS.
  //
  // Best-effort: failure logs an error but doesn't block — the
  // slow-path `prepareChapterHtml` fallback further down catches
  // the corner cases (e.g. ZIP read error, HtmlStripper choke).
  // In default env (SNAPIX_MARKERIZER=0) this is a stub and we go
  // straight to the slow path.
  // Keep one directory snapshot for this parse operation.  Writes update the
  // snapshot in place, and the object is released before parsePages returns.
  auto ucache = snapix::unifiedcache::UnifiedCache::shared(epub_->getCachePath());
  (void)tryMarkerizeChapter(ucache);

#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  const auto streamingCfg = makeEpubStreamingConfig(
      renderer_, config_, streamingViewportMarginTop_,
      streamingViewportMarginBottom_, streamingViewportMarginLeft_,
      streamingViewportMarginRight_, epub_->getLanguage());
  const uint16_t streamingConfigHash =
      snapix::smolport::computePageIndexConfigHash(
          streamingCfg, config_.fontId, config_.fakeBold);

  auto hasCurrentIndex = [&]() -> bool {
    File idxProbe;
    size_t idxProbeSize = 0;
    if (!ucache.openSegmentReader(snapix::unifiedcache::Kind::Idx,
                                  static_cast<uint16_t>(spineIndex_), idxProbe,
                                  &idxProbeSize)) {
      return false;
    }
    uint8_t h[8] = {};
    const bool readOk =
        idxProbeSize >= sizeof(h) &&
        idxProbe.read(h, sizeof(h)) == static_cast<int>(sizeof(h));
    idxProbe.close();
    if (!readOk) return false;
    const bool magicOk =
        h[0] == 0x53 && h[1] == 0x50 && h[2] == 0x49 && h[3] == 0x58;
    const uint16_t version =
        static_cast<uint16_t>(h[4]) | (static_cast<uint16_t>(h[5]) << 8);
    const uint16_t configHash =
        static_cast<uint16_t>(h[6]) | (static_cast<uint16_t>(h[7]) << 8);
    return magicOk && version == snapix::smolport::kPageIndexVersion &&
           configHash == streamingConfigHash;
  };

  // A cold TOC jump requests exactly one page so the target chapter can be
  // displayed while its exact anchor is resolved.  The old flow still ran the
  // full MEASURE walk before emitting that page (80.9 s in the hardware log).
  // Once markerization has atomically published a readable stream, page 0 can
  // render without an idx; leave the parser resumable so the next worker pass
  // builds the complete index and anchor map in the background.
  size_t markerBytesForBootstrap = 0;
  const bool markersReady =
      ucache.segmentSize(snapix::unifiedcache::Kind::Markers,
                         static_cast<uint16_t>(spineIndex_),
                         &markerBytesForBootstrap) &&
      markerBytesForBootstrap > 0;
  if (maxPages == 1 && !bootstrapPageEmitted_ && markersReady &&
      !hasCurrentIndex()) {
    onPageComplete(std::unique_ptr<Page>(new Page));
    bootstrapPageEmitted_ = true;
    awaitingIndexBuild_ = true;
    initialized_ = true;
    hasMore_ = true;
    LOG_INF(TAG,
            "[CONTENT][EPUB] [STREAM] bootstrap page emitted before idx "
            "spine=%d markerBytes=%u",
            spineIndex_, static_cast<unsigned>(markerBytesForBootstrap));
    return true;
  }
  awaitingIndexBuild_ = false;

  // v2.0.130 R4.c — build `.idx` upfront via MEASURE-only walk so
  // R4.b short-circuit fires on FIRST visit too, not just on repeat
  // visits.  Without this step, the very first cold-extend of a
  // chapter still runs the legacy ChapterHtmlSlimParser even though
  // markers exist; with it, the parser is skipped from byte 0 of
  // the chapter onward.
  //
  // Cost on first visit: one streaming walk through the entire
  // markers stream with paginator in MEASURE-only mode (no draws).
  // Captures every page boundary via callback; ~50-150 ms per
  // chapter (vs 1500-3000 ms for legacy parser walk).
  //
  // Skip when:
  //   * `.idx` already exists (next R4.b block will short-circuit)
  //   * markers file missing (markerize failed earlier)
  //
  // Use a "best-guess" paginator config (full screen, default
  // 4-px margins) since the parser doesn't know the user's actual
  // viewport margins yet.  Result: pageCount may differ by ±1 from
  // the eventual render-time config.  R3 streaming-render path
  // detects the configHash mismatch and rebuilds `.idx` correctly
  // on first render — but the count from this upfront build is
  // sufficient to skip the legacy parser at cold-extend time, and
  // user sees correct counts after one render pass.
  do {
    // v2.0.167 — markers + idx in UnifiedCache.  R4.c only runs if markers
    // exist AND idx doesn't (idx is built once per (spine,config) tuple).
    size_t markersSize = 0;
    if (!ucache.segmentSize(snapix::unifiedcache::Kind::Markers,
                             static_cast<uint16_t>(spineIndex_), &markersSize)) {
      break;  // markerize failed earlier
    }
    // v2.0.206 — skip the upfront rebuild ONLY when a CURRENT-version idx
    // already exists.  A stale idx (older kPageIndexVersion, e.g. after the
    // 9→10 page-count-fix bump) is rejected by R4.b's version gate, so it
    // MUST be rebuilt here.  Pre-fix this skipped on mere existence and
    // relied on the render path to drop a stale idx — but the render path
    // never runs while this page-count cache build is still failing on the
    // stale idx, so the build could wedge.  Rebuilding here is self-healing:
    // the writeSegment at the end of this block appends a fresh frame that
    // supersedes the stale one (UnifiedCache is an append-log, latest frame
    // per key wins).  Probe is cheap — read only the 6-byte magic+version.
    if (hasCurrentIndex()) {
      break;  // up-to-date idx exists — R4.b will short-circuit on it
    }
    File mf;
    size_t markersStreamSize = 0;
    if (!ucache.openSegmentReader(snapix::unifiedcache::Kind::Markers,
                                    static_cast<uint16_t>(spineIndex_), mf, &markersStreamSize)) {
      break;
    }

    // v2.0.131 — paginator config from renderer + fontId + real viewport
    // margins plumbed via setStreamingViewport().  MUST match the config
    // that ReaderState::renderPageContents builds, else (a) the .idx
    // configHash will mismatch and the render path discards our work,
    // (b) the pageCount we write here will differ from what the renderer
    // produces — R4.b would then emit empty Pages for nonexistent pages
    // and the legacy fallback path renders them as white screen.
    // Shared with the compatibility anchor scanner so an existing idx is
    // accepted only for the exact same viewport/font/layout configuration.
    const auto& cfg = streamingCfg;

    // v2.0.136 — diagnostic log: dump paginator config so we can verify
    // it matches what ReaderState::renderPageContents uses at render
    // time.  Half-page rendering issues so far traced back to config
    // mismatch (margins, screen dimensions).  This log makes the
    // mismatch visible without needing extra debug builds.
    LOG_INF(TAG,
            "[CONTENT][EPUB] [STREAM] R4.c paginator cfg spine=%d fontId=%d "
            "pageW=%u pageH=%u mT=%u mB=%u mL=%u mR=%u bodyLH=%u",
            spineIndex_, config_.fontId,
            static_cast<unsigned>(cfg.pageWidth), static_cast<unsigned>(cfg.pageHeight),
            static_cast<unsigned>(cfg.marginTop), static_cast<unsigned>(cfg.marginBottom),
            static_cast<unsigned>(cfg.marginLeft), static_cast<unsigned>(cfg.marginRight),
            static_cast<unsigned>(cfg.bodyLineHeight));

    // v2.0.145 — image resolver so the MEASURE walk accounts for
    // image heights and produces correct page boundaries.  Reuses
    // the cached BMP path convention from
    // ChapterHtmlSlimParser::cacheImage so the streaming render
    // path sees the same images.
    const std::string imageCacheDir = imageCachePath_;
    const std::string chapterBase = chapterBasePath_;
    auto resolveImage = [imageCacheDir, chapterBase](const uint8_t* p,
                                                      size_t l) -> std::string {
      if (imageCacheDir.empty() || p == nullptr || l == 0) return {};
      std::string src(reinterpret_cast<const char*>(p), l);
      std::string resolved = chapterBase.empty()
                                 ? src
                                 : FsHelpers::normalisePath(chapterBase + src);
      const size_t srcHash = std::hash<std::string>{}(resolved);
      return imageCacheDir + "/" + std::to_string(srcHash) + ".bmp";
    };
    // v2.0.146 — pass fakeBold so the MEASURE-walk uses the SAME
    // word widths as runtime render (which also has fakeBold wired
    // in via ReaderState).  Mismatched fakeBold between MEASURE and
    // DRAW would produce different page boundaries at runtime than
    // the .idx records — same bug class as configHash mismatch.
    snapix::smolport::GfxRendererPaginatorAdapter adapter(renderer_, config_.fontId, config_.fontId,
                                                            true, resolveImage,
                                                            config_.fakeBold,
                                                            config_.superSubFontId);  // v3.6.0
    snapix::smolport::StreamingPaginator paginator(cfg, adapter);

    // v3.10.2 — HEAP the streaming buffer (was a 4 KB stack array).  This R4.c
    // MEASURE walk runs on the 20 KB ReaderAsync task and reads BMP image
    // headers via resolveImageSize during the walk; a 4 KB stack frame here (on
    // top of the serdebuf below) is needless stack pressure on the same task
    // that overflowed in v3.10.1 for FB2.  nothrow → degrade, never crash.
    constexpr size_t kChunkBufBytes = 4096;
    std::unique_ptr<uint8_t[]> chunkBuf(new (std::nothrow) uint8_t[kChunkBufBytes]);
    if (!chunkBuf) {
      LOG_ERR(TAG, "[CONTENT][EPUB] [STREAM] R4.c chunkBuf alloc failed spine=%d", spineIndex_);
      mf.close();
      break;
    }
    // v2.0.167 — markers reader is positioned inside streaming.cache and
    // would read past the segment into the next frame's bytes if unbounded.
    // Track remaining bytes from segmentSize and clamp each read.
    size_t markersRemaining = markersStreamSize;
    auto readFn = [&mf, &markersRemaining](uint8_t* buf, size_t bufSize) -> int {
      if (!mf || markersRemaining == 0) return markersRemaining == 0 ? 0 : -1;
      const size_t want = std::min(bufSize, markersRemaining);
      const int got = mf.read(buf, want);
      if (got > 0) markersRemaining -= static_cast<size_t>(got);
      return got;
    };

    std::vector<snapix::smolport::PageBoundarySnapshot> captured;
    auto captureFn = [&captured](const snapix::smolport::PageBoundarySnapshot& s) {
      captured.push_back(s);
    };
    anchorMap_.clear();
    auto captureAnchorFn =
        [this](const uint8_t* id, uint16_t len, uint16_t pageIndex) {
          if (id == nullptr || len == 0) return;
          anchorMap_.emplace_back(
              std::string(reinterpret_cast<const char*>(id), len), pageIndex);
        };

    // Target page UINT16_MAX is never reached; walks the whole
    // stream in draw-suppressed mode and fires the callback at
    // every page boundary.  Returns PageNotFound when EOF hits.
    snapix::smolport::MarkerizedRenderStats stats{};
    const auto indexStatus = snapix::smolport::renderMarkerizedPage(
        paginator, readFn, chunkBuf.get(), kChunkBufBytes, UINT16_MAX,
        shouldAbort, &stats, {}, captureFn, captureAnchorFn);
    mf.close();

    if (indexStatus == snapix::smolport::MarkerizedRenderResult::Aborted) {
      // Never publish a prefix as a complete idx.  Preserve the bootstrap
      // parser state so a later idle worker can restart the index walk while
      // the already-visible page remains usable.
      awaitingIndexBuild_ = bootstrapPageEmitted_;
      initialized_ = bootstrapPageEmitted_;
      hasMore_ = true;
      anchorMap_.clear();
      LOG_INF(TAG,
              "[CONTENT][EPUB] [STREAM] idx build cancelled spine=%d "
              "bytes=%u boundaries=%u",
              spineIndex_, static_cast<unsigned>(stats.bytesConsumed),
              static_cast<unsigned>(captured.size()));
      return false;
    }
    if (indexStatus == snapix::smolport::MarkerizedRenderResult::ReadError) {
      anchorMap_.clear();
      LOG_ERR(TAG,
              "[CONTENT][EPUB] [STREAM] idx build read error spine=%d",
              spineIndex_);
      break;
    }

    // v2.0.206 — page-count semantics: `captured` holds the OVERFLOW page
    // boundaries only (page N filled → roll into page N+1).  A chapter
    // spanning P pages therefore produces exactly P-1 boundaries: page 0 is
    // implicit (starts at byte 0, fires no boundary) and the final partial
    // page ends at EOF (fires no boundary either).  So `captured.size()`
    // here is (pageCount - 1).  R4.b reconstitutes the true count as
    // `boundaryCount + 1` when it emits Pages.
    //
    // A single-page chapter (content fits in less than one rendered page)
    // produces ZERO boundaries — `captured` is legitimately empty and we
    // serialize a 0-entry idx (just the header); R4.b emits 0 + 1 = 1 Page.
    // (v2.0.175 used to synthesise a fake page-0 entry here; that made the
    // count 1 for single-page chapters but, combined with R4.b's old
    // `emit entryCount` logic, masked the multi-page off-by-one.  With the
    // `+1` reconstitution the synthesis is both unnecessary and wrong, so
    // it's removed.)  Only a genuinely empty/corrupt markers stream
    // (0 bytes consumed) is an error.
    if (captured.empty() && stats.bytesConsumed == 0) {
      LOG_ERR(TAG,
              "[CONTENT][EPUB] [STREAM] R4.c read 0 bytes from markers spine=%d "
              "size=%zu — markers segment is empty/corrupt",
              spineIndex_, markersStreamSize);
      break;
    }

    // Serialize captured boundaries into `.idx` via SmolPort helper.
    // Use the same config-hash as we just used so the streaming
    // render path can hit our upfront-built index without rebuild.
    const uint16_t configHash = streamingConfigHash;
    // v3.10.2 — HEAP the idx serialize buffer (was a 4 KB stack array with a
    // `needed > 4096` cap that SILENTLY dropped the idx for >~500-page chapters,
    // forcing a render-time cold walk).  Heap `needed` exactly (nothrow): no
    // stack pressure, and large chapters now get a full upfront idx.
    const size_t needed = snapix::smolport::kPageIndexHeaderBytes +
                           captured.size() * snapix::smolport::kPageIndexEntryBytes;
    std::unique_ptr<uint8_t[]> serdebuf(new (std::nothrow) uint8_t[needed]);
    if (!serdebuf) {
      LOG_ERR(TAG, "[CONTENT][EPUB] [STREAM] idx alloc failed spine=%d bytes=%zu", spineIndex_, needed);
      break;
    }
    const size_t wrote = snapix::smolport::serializePageIndex(
        captured.data(), captured.size(), configHash, serdebuf.get(), needed);
    if (wrote == 0) break;

    // v2.0.167 — write idx as a UnifiedCache::Idx segment.  No more
    // .work + rename — UnifiedCache append is atomic at the frame level.
    if (!ucache.writeSegment(snapix::unifiedcache::Kind::Idx,
                              static_cast<uint16_t>(spineIndex_), serdebuf.get(), wrote)) {
      LOG_ERR(TAG, "[CONTENT][EPUB] [STREAM] idx write to UnifiedCache failed spine=%d", spineIndex_);
      break;
    }
    LOG_INF(TAG,
            "[CONTENT][EPUB] [STREAM] idx built upfront spine=%d pages=%u "
            "anchors=%u bytes=%u (skipping legacy parser)",
            spineIndex_, static_cast<unsigned>(captured.size() + 1),
            static_cast<unsigned>(anchorMap_.size()),
            static_cast<unsigned>(stats.bytesConsumed));
  } while (false);
#endif

  // v2.0.129 R4.b — skip legacy ChapterHtmlSlimParser entirely when a
  // `.idx` sidecar exists for this spine.  The .idx (R4.a) records
  // the page count from a previous streaming-render session; emit
  // that many EMPTY Page objects and skip the heap-heavy parser.
  // The streaming render path (R3.5) ignores Page contents when
  // markers exist — it reads pixels straight from the markers
  // stream — so empty pages here cost ~64 bytes each and produce
  // the same visual output as full Page-tree pages would.
  //
  // Heap impact: legacy parser used 12-15 KB peak for ChapterHtml
  // SlimParser + Page tree + TextBlock pool.  Empty-pages emit
  // path uses ~64 B per page × ~50 pages = ~3 KB, all on stack via
  // unique_ptr<Page>{} that the cache writer immediately consumes.
  // Eliminates the "Aborting job: heap dangerously low" class of
  // failures during cold extends.
  //
  // Falls back to legacy parser if:
  //   * `.idx` doesn't exist (first-ever visit to this spine)
  //   * `.idx` header is malformed
  //   * `.idx` pageCount is 0 (would emit zero pages — useless)
  //
  // Future R4.c: build the .idx upfront via MEASURE-only walk
  // during cold extend, eliminating the legacy parser dependency
  // even on first visit.
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  do {
    // v2.0.167 — read idx from UnifiedCache::Idx segment.  Only need
    // the 12-byte header (magic + version + pageCount), not the body —
    // use a streaming reader so we don't allocate for the full segment.
    File idxF;
    size_t idxSegSize = 0;
    if (!ucache.openSegmentReader(snapix::unifiedcache::Kind::Idx,
                                    static_cast<uint16_t>(spineIndex_), idxF, &idxSegSize)) {
      break;
    }
    if (idxSegSize < 12) {
      idxF.close();
      break;
    }
    uint8_t header[12];
    const int n = idxF.read(header, sizeof(header));
    idxF.close();
    if (n != static_cast<int>(sizeof(header))) break;
    // Magic check: bytes 0-3 must be "SPIX" (0x53, 0x50, 0x49, 0x58).
    if (header[0] != 0x53 || header[1] != 0x50 || header[2] != 0x49 || header[3] != 0x58) break;
    // Version at offset 4-5 (uint16 LE).  Compare against the canonical
    // constant from MarkerizedPageRender.h so a version bump
    // automatically rejects stale on-disk indices (e.g. v2.0.134
    // bumped 1→2 to invalidate pre-fix paginator's offsets).
    const uint16_t version = static_cast<uint16_t>(header[4]) | (static_cast<uint16_t>(header[5]) << 8);
    if (version != snapix::smolport::kPageIndexVersion) break;
    const uint16_t configHash =
        static_cast<uint16_t>(header[6]) |
        (static_cast<uint16_t>(header[7]) << 8);
    if (configHash != streamingConfigHash) break;
    // Boundary count at offset 8-9 (uint16 LE).  v2.0.206 — this is the
    // number of OVERFLOW page rolls, i.e. (true page count - 1): page 0 is
    // implicit and the final partial page fires no boundary.  Reconstitute
    // the true page count by adding 1.  A 0-boundary idx is a valid
    // single-page chapter (emits 1 Page), so no `== 0` early-out.
    const uint16_t boundaryCount = static_cast<uint16_t>(header[8]) | (static_cast<uint16_t>(header[9]) << 8);
    const uint16_t totalPages = static_cast<uint16_t>(boundaryCount + 1);

    LOG_INF(TAG, "[CONTENT][EPUB] [STREAM] short-circuit: total %u pages from .idx spine=%d (skipping legacy parser)",
            static_cast<unsigned>(totalPages), spineIndex_);

    // v2.0.132 — activate STATEFUL short-circuit.  Emit up to maxPages
    // in this call; remaining pages are emitted on subsequent
    // parsePages calls via the shortCircuitActive_ branch at the top.
    // Setting initialized_=true (rather than false) keeps the parser
    // alive from the cache's perspective and ensures `canResume()`
    // returns true so cache.extend() uses the hot-extend path.
    onPageComplete_ = onPageComplete;
    maxPages_ = maxPages;
    pagesCreated_ = 0;
    hitMaxPages_ = false;
    shortCircuitActive_ = true;
    shortCircuitTotalPages_ = totalPages;
    // Page 0 may already be present in the PageCache from the cold-TOC
    // bootstrap pass.  Continue at page 1 so hot extend never appends a
    // duplicate placeholder and shifts every later page by one.
    shortCircuitNextPage_ =
        bootstrapPageEmitted_ ? std::min<uint16_t>(1, totalPages) : 0;
    bootstrapPageEmitted_ = false;

    while (shortCircuitNextPage_ < shortCircuitTotalPages_) {
      if (maxPages > 0 && pagesCreated_ >= maxPages) break;
      onPageComplete(std::unique_ptr<Page>(new Page));
      pagesCreated_++;
      shortCircuitNextPage_++;
    }
    hasMore_ = (shortCircuitNextPage_ < shortCircuitTotalPages_);
    if (!hasMore_) {
      // Single batch was enough to drain .idx — clean up.
      shortCircuitActive_ = false;
      initialized_ = false;
      renderer_.clearWidthCache();
    } else {
      // Keep state alive for next parsePages call.  initialized_ stays
      // true so canResume() returns true.
      initialized_ = true;
    }
    LOG_INF(TAG, "[CONTENT][EPUB] [STREAM] short-circuit batch spine=%d pagesCreated=%u nextPage=%u total=%u hasMore=%u",
            spineIndex_, static_cast<unsigned>(pagesCreated_),
            static_cast<unsigned>(shortCircuitNextPage_),
            static_cast<unsigned>(shortCircuitTotalPages_), static_cast<unsigned>(hasMore_));
    // A genuinely one-page chapter reaches completion without appending a
    // page here because bootstrap already emitted its sole page.  Returning
    // true lets PageCache mark the hot extend complete with zero growth.
    return pagesCreated_ > 0 || !hasMore_;
  } while (false);
#endif

  // v2.0.161 — fast path didn't fire (`SNAPIX_MARKERIZER=0` in default
  // env, or markerize/idx-build/short-circuit all failed in v3_alpha).
  // The legacy ChapterHtmlSlimParser fallback that used to run here is
  // gone.  Reaching this point is an unhandled error in v3_alpha; we
  // return false so the cache controller knows no pages were produced.
  LOG_ERR(TAG, "[CONTENT][EPUB] parsePages reached end with no streaming output spine=%d "
               "(markerize/idx failed and legacy fallback was removed in v2.0.161)",
          spineIndex_);
  hasMore_ = false;
  initialized_ = false;
  return false;
}
