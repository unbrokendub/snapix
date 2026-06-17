#include "Fb2Parser.h"

#include "Fb2.h"
#include <GfxRenderer.h>
#include <Logging.h>
#include <Page.h>
#include <SDCardManager.h>
#include <SharedSpiLock.h>
#include <UnifiedCache.h>  // v2.0.167 — Phase 5 unified streaming cache

#include <algorithm>
#include <cstring>
#include <utility>

// v3.8.0 — FB2 is now streaming-only (legacy Expat/ParsedText path removed).
// The markerize side-channel + R4.c idx build + R4.b short-circuit are the only
// ingestion path, mirroring EpubChapterParser.  Compiled out (no pages) when
// SNAPIX_MARKERIZER=0 (build-test only).
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
#include <FS.h>
#include <Fb2Stripper.h>
#include <GfxRendererPaginatorAdapter.h>
#include <LittleFS.h>
#include <MarkerizedPageRender.h>
#include <MarkerizeChapter.h>
#include <StreamingPaginator.h>

#include <memory>
#include <new>
#endif

#define TAG "FB2_PARSE"

Fb2Parser::Fb2Parser(std::string filepath, GfxRenderer& renderer, const RenderConfig& config, const uint32_t startOffset,
                     const int startingSectionIndex, const bool sectionScoped, const uint32_t endOffset)
    : filepath_(std::move(filepath)),
      renderer_(renderer),
      config_(config),
      startOffset_(startOffset),
      endOffset_(endOffset),
      startingSectionIndex_(startingSectionIndex),
      sectionScoped_(sectionScoped) {}

Fb2Parser::~Fb2Parser() = default;

void Fb2Parser::reset() {
  hasMore_ = true;
  markerizeAttempted_ = false;
  shortCircuitActive_ = false;
  shortCircuitTotalPages_ = 0;
  shortCircuitNextPage_ = 0;
}

// =============================================================================
// markerize side-channel for FB2 — best-effort.  Streams the FB2 source's
// [startOffset_..endOffset_) byte range (or the whole file for a no-TOC, whole-
// book parser) through the Fb2-mode HtmlStripper into a UnifiedCache::Markers
// segment keyed by startingSectionIndex_.  Failure logs but doesn't throw.
// =============================================================================
bool Fb2Parser::tryMarkerizeSection() {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  if (!fb2_ || filepath_.empty()) return false;

  // v2.0.167 — markers live as a UnifiedCache::Markers segment in
  // <fb2CachePath>/streaming.cache, keyed by sectionIndex.
  auto cache = snapix::unifiedcache::UnifiedCache::shared(fb2_->getCachePath());
  size_t existingSize = 0;
  if (cache.segmentSize(snapix::unifiedcache::Kind::Markers,
                         static_cast<uint16_t>(startingSectionIndex_), &existingSize)) {
    LOG_INF(TAG, "[CONTENT][FB2] markerize cache hit section=%d (UnifiedCache::Markers size=%zu)",
            startingSectionIndex_, existingSize);
    return true;
  }

  // Open SD source separately so we own the cursor + bus lock window.
  FsFile srcFile;
  if (!SdMan.openFileForRead("FB2_M", filepath_, srcFile)) {
    LOG_ERR(TAG, "[CONTENT][FB2] markerize open source failed path=%s", filepath_.c_str());
    return false;
  }
  if (startOffset_ > 0) {
    snapix::spi::SharedBusLock lk;
    srcFile.seek(startOffset_);
  }

  constexpr size_t kChunkBufBytes = 4096;
  uint8_t chunkBuf[kChunkBufBytes];
  uint32_t bytesRemaining = (sectionScoped_ && endOffset_ > startOffset_)
                                ? (endOffset_ - startOffset_)
                                : UINT32_MAX;

  snapix::smolport::MarkerizeStats stats{};
  snapix::smolport::MarkerizeStatus status = snapix::smolport::MarkerizeStatus::ReadError;
  const bool ok = cache.writeSegmentStreamingDeferred(
      snapix::unifiedcache::Kind::Markers, static_cast<uint16_t>(startingSectionIndex_),
      [&](File& outFile) -> bool {
        auto readFn = [&srcFile, &bytesRemaining](uint8_t* buf, size_t bufSize) -> int {
          if (bytesRemaining == 0) return 0;
          int n;
          {
            snapix::spi::SharedBusLock lk;
            if (!srcFile.available()) return 0;
            size_t toRead = bufSize;
            if (toRead > bytesRemaining) toRead = bytesRemaining;
            n = srcFile.read(buf, toRead);
          }
          if (n < 0) return -1;
          if (n > 0) bytesRemaining -= static_cast<uint32_t>(n);
          return n;
        };
        auto writeFn = [&outFile](const uint8_t* data, size_t len) -> bool {
          if (!outFile) return false;
          const size_t n = outFile.write(data, len);
          return n == len;
        };
        status = snapix::smolport::markerizeChapter(
            snapix::smolport::HtmlStripper::Mode::Fb2, readFn, writeFn, chunkBuf, sizeof(chunkBuf),
            {}, &stats);
        return status == snapix::smolport::MarkerizeStatus::Success;
      });

  srcFile.close();

  if (!ok || status != snapix::smolport::MarkerizeStatus::Success) {
    LOG_ERR(TAG,
            "[CONTENT][FB2] markerize failed section=%d status=%u in=%u out=%u chunks=%u (UnifiedCache ok=%u)",
            startingSectionIndex_, static_cast<unsigned>(status),
            static_cast<unsigned>(stats.inputBytes), static_cast<unsigned>(stats.outputBytes),
            static_cast<unsigned>(stats.chunksProcessed), static_cast<unsigned>(ok));
    return false;
  }

  LOG_INF(TAG,
          "[CONTENT][FB2] markerize done section=%d in=%u out=%u chunks=%u (UnifiedCache::Markers)",
          startingSectionIndex_, static_cast<unsigned>(stats.inputBytes),
          static_cast<unsigned>(stats.outputBytes), static_cast<unsigned>(stats.chunksProcessed));
  return true;
#else
  return true;
#endif
}

bool Fb2Parser::parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages,
                           const AbortCallback& shouldAbort) {
  (void)shouldAbort;  // markerize/short-circuit are not interrupted mid-pass
  onPageComplete_ = onPageComplete;
  maxPages_ = maxPages;
  pagesCreated_ = 0;

#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  // R4.b STATEFUL short-circuit continuation — drain remaining empty Pages
  // from a previously-read idx page count (cache batches with maxPages).
  if (shortCircuitActive_) {
    const uint16_t startPage = shortCircuitNextPage_;
    while (shortCircuitNextPage_ < shortCircuitTotalPages_) {
      if (maxPages > 0 && pagesCreated_ >= maxPages) break;
      onPageComplete_(std::unique_ptr<Page>(new Page));
      pagesCreated_++;
      shortCircuitNextPage_++;
    }
    hasMore_ = (shortCircuitNextPage_ < shortCircuitTotalPages_);
    if (!hasMore_) {
      shortCircuitActive_ = false;
      renderer_.clearWidthCache();
    }
    LOG_INF(TAG, "[CONTENT][FB2] [STREAM] short-circuit batch section=%d emitted=%u..%u of %u hasMore=%u",
            startingSectionIndex_, static_cast<unsigned>(startPage),
            static_cast<unsigned>(shortCircuitNextPage_),
            static_cast<unsigned>(shortCircuitTotalPages_), static_cast<unsigned>(hasMore_));
    return pagesCreated_ > 0;
  }

  // INIT: markerize the section, then build the idx, then short-circuit.
  if (!markerizeAttempted_) {
    markerizeAttempted_ = true;
    (void)tryMarkerizeSection();
  }

  // v2.0.130 R4.c — build `.idx` upfront via a MEASURE-only walk so the R4.b
  // short-circuit fires on the FIRST visit too.  See EpubChapterParser for the
  // full rationale.  Skips when a current-version idx already exists.
  if (fb2_) {
    auto ucache = snapix::unifiedcache::UnifiedCache::shared(fb2_->getCachePath());
    do {
      {
        File idxProbe;
        size_t idxProbeSize = 0;
        bool idxCurrent = false;
        if (ucache.openSegmentReader(snapix::unifiedcache::Kind::Idx,
                                     static_cast<uint16_t>(startingSectionIndex_), idxProbe,
                                     &idxProbeSize)) {
          uint8_t h[6];
          if (idxProbeSize >= sizeof(h) &&
              idxProbe.read(h, sizeof(h)) == static_cast<int>(sizeof(h))) {
            const bool magicOk = h[0] == 0x53 && h[1] == 0x50 && h[2] == 0x49 && h[3] == 0x58;
            const uint16_t ver = static_cast<uint16_t>(h[4]) | (static_cast<uint16_t>(h[5]) << 8);
            idxCurrent = magicOk && ver == snapix::smolport::kPageIndexVersion;
          }
          idxProbe.close();
        }
        if (idxCurrent) break;  // up-to-date idx — R4.b handles it
      }
      File mf;
      size_t markersStreamSize = 0;
      if (!ucache.openSegmentReader(snapix::unifiedcache::Kind::Markers,
                                      static_cast<uint16_t>(startingSectionIndex_), mf,
                                      &markersStreamSize)) {
        break;  // markerize hasn't produced markers yet
      }

      const uint16_t bodyLineH =
          static_cast<uint16_t>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression);
      snapix::smolport::StreamingPaginatorConfig cfg{};
      cfg.pageWidth = static_cast<uint16_t>(renderer_.getScreenWidth());
      cfg.pageHeight = static_cast<uint16_t>(renderer_.getScreenHeight());
      cfg.marginTop = static_cast<uint16_t>(streamingViewportMarginTop_);
      cfg.marginBottom = static_cast<uint16_t>(streamingViewportMarginBottom_);
      cfg.marginLeft = static_cast<uint16_t>(streamingViewportMarginLeft_);
      cfg.marginRight = static_cast<uint16_t>(streamingViewportMarginRight_);
      cfg.bodyLineHeight = bodyLineH > 0 ? bodyLineH : 24;
      cfg.headingLineHeight = static_cast<uint16_t>(cfg.bodyLineHeight * 3 / 2);
      cfg.paragraphSpacing =
          snapix::smolport::paragraphSpacingForLevel(config_.spacingLevel, cfg.bodyLineHeight);
      cfg.firstLineIndent =
          snapix::smolport::firstLineIndentForLevel(config_.indentLevel, cfg.bodyLineHeight);
      // FB2 is the Russian ebook format → default hyphenation to "ru" (Cyrillic-
      // only patterns; a Latin FB2 simply gets no hyphenation).  MUST match
      // ReaderState's render-time cfg for this section.
      cfg.hyphenate = true;
      std::strncpy(cfg.hyphenLang, "ru", sizeof(cfg.hyphenLang) - 1);
      cfg.hyphenLang[sizeof(cfg.hyphenLang) - 1] = '\0';

      LOG_INF(TAG,
              "[CONTENT][FB2] [STREAM] R4.c paginator cfg section=%d fontId=%d "
              "pageW=%u pageH=%u mT=%u mB=%u mL=%u mR=%u bodyLH=%u",
              startingSectionIndex_, config_.fontId,
              static_cast<unsigned>(cfg.pageWidth), static_cast<unsigned>(cfg.pageHeight),
              static_cast<unsigned>(cfg.marginTop), static_cast<unsigned>(cfg.marginBottom),
              static_cast<unsigned>(cfg.marginLeft), static_cast<unsigned>(cfg.marginRight),
              static_cast<unsigned>(cfg.bodyLineHeight));

      // Image resolver for FB2 — lazy decode-on-demand so MEASURE-walk page
      // boundaries account for image heights (cacheImage is idempotent).
      const Fb2* fb2Ptr = fb2_;
      const uint16_t imgMaxW =
          static_cast<uint16_t>(cfg.pageWidth - cfg.marginLeft - cfg.marginRight);
      const uint16_t imgMaxH =
          static_cast<uint16_t>(cfg.pageHeight - cfg.marginTop - cfg.marginBottom);
      auto resolveImage = [fb2Ptr, imgMaxW, imgMaxH](const uint8_t* p, size_t l) -> std::string {
        if (fb2Ptr == nullptr || p == nullptr || l == 0) return {};
        std::string src(reinterpret_cast<const char*>(p), l);
        if (!src.empty() && src[0] == '#') src.erase(0, 1);
        std::string outPath;
        uint16_t w = 0, h = 0;
        const bool ok = fb2Ptr->cacheImage(src, outPath, w, h, imgMaxW, imgMaxH,
                                            /*fastMode=*/true, /*shouldAbort=*/{});
        return ok ? outPath : std::string();
      };
      snapix::smolport::GfxRendererPaginatorAdapter adapter(renderer_, config_.fontId, config_.fontId,
                                                            true, resolveImage, config_.fakeBold,
                                                            config_.superSubFontId);
      snapix::smolport::StreamingPaginator paginator(cfg, adapter);

      constexpr size_t kChunkBufBytes = 4096;
      uint8_t chunkBuf[kChunkBufBytes];
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

      snapix::smolport::MarkerizedRenderStats stats{};
      (void)snapix::smolport::renderMarkerizedPage(paginator, readFn, chunkBuf, sizeof(chunkBuf),
                                                    UINT16_MAX, {}, &stats, {}, captureFn);
      mf.close();

      if (captured.empty() && stats.bytesConsumed == 0) {
        LOG_ERR(TAG,
                "[CONTENT][FB2] [STREAM] R4.c read 0 bytes from markers section=%d size=%zu "
                "(empty/corrupt)",
                startingSectionIndex_, markersStreamSize);
        break;
      }

      const uint16_t configHash =
          snapix::smolport::computePageIndexConfigHash(cfg, config_.fontId, config_.fakeBold);
      // v3.8.0 — a whole-book (no-TOC) FB2 is one big section, so the idx can
      // exceed the 4 KB stack buffer; persist the FULL idx via a heap buffer
      // (nothrow), like StreamingSection.  Partial-idx persistence would make a
      // later open mistake a prefix for the whole page count (stuck pagination).
      const size_t needed = snapix::smolport::kPageIndexHeaderBytes +
                            captured.size() * snapix::smolport::kPageIndexEntryBytes;
      std::unique_ptr<uint8_t[]> serdebuf(new (std::nothrow) uint8_t[needed]);
      if (!serdebuf) {
        LOG_ERR(TAG, "[CONTENT][FB2] [STREAM] idx alloc failed section=%d bytes=%zu",
                startingSectionIndex_, needed);
        break;
      }
      const size_t wrote = snapix::smolport::serializePageIndex(
          captured.data(), captured.size(), configHash, serdebuf.get(), needed);
      if (wrote == 0) break;

      if (!ucache.writeSegment(snapix::unifiedcache::Kind::Idx,
                                static_cast<uint16_t>(startingSectionIndex_), serdebuf.get(), wrote)) {
        LOG_ERR(TAG, "[CONTENT][FB2] [STREAM] idx write to UnifiedCache failed section=%d",
                startingSectionIndex_);
        break;
      }
      LOG_INF(TAG,
              "[CONTENT][FB2] [STREAM] idx built upfront section=%d pages=%u bytes=%zu",
              startingSectionIndex_, static_cast<unsigned>(captured.size() + 1), wrote);
    } while (false);
  }

  // v2.0.129 R4.b — short-circuit: read the idx page count and emit that many
  // empty Page objects.  No legacy fallback — if no idx exists (markerize/idx
  // failed, which is I/O-only), the section produces no pages.
  if (fb2_) {
    auto ucache = snapix::unifiedcache::UnifiedCache::shared(fb2_->getCachePath());
    do {
      File idxF;
      size_t idxSegSize = 0;
      if (!ucache.openSegmentReader(snapix::unifiedcache::Kind::Idx,
                                      static_cast<uint16_t>(startingSectionIndex_), idxF, &idxSegSize)) {
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
      if (header[0] != 0x53 || header[1] != 0x50 || header[2] != 0x49 || header[3] != 0x58) break;
      const uint16_t version = static_cast<uint16_t>(header[4]) | (static_cast<uint16_t>(header[5]) << 8);
      if (version != snapix::smolport::kPageIndexVersion) break;
      const uint16_t boundaryCount = static_cast<uint16_t>(header[8]) | (static_cast<uint16_t>(header[9]) << 8);
      const uint16_t totalPages = static_cast<uint16_t>(boundaryCount + 1);

      LOG_INF(TAG, "[CONTENT][FB2] [STREAM] short-circuit: total %u pages from .idx section=%d",
              static_cast<unsigned>(totalPages), startingSectionIndex_);

      shortCircuitActive_ = true;
      shortCircuitTotalPages_ = totalPages;
      shortCircuitNextPage_ = 0;
      while (shortCircuitNextPage_ < shortCircuitTotalPages_) {
        if (maxPages > 0 && pagesCreated_ >= maxPages) break;
        onPageComplete_(std::unique_ptr<Page>(new Page));
        pagesCreated_++;
        shortCircuitNextPage_++;
      }
      hasMore_ = (shortCircuitNextPage_ < shortCircuitTotalPages_);
      if (!hasMore_) {
        shortCircuitActive_ = false;
        renderer_.clearWidthCache();
      }
      return pagesCreated_ > 0;
    } while (false);
  }

  // No idx → no pages (no legacy fallback in v3.8.0).
  hasMore_ = false;
  return false;
#else
  (void)onPageComplete;
  (void)maxPages;
  hasMore_ = false;
  return false;
#endif
}
