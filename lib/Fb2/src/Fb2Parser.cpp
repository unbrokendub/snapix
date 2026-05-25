#include "Fb2Parser.h"

#include "Fb2.h"
#include <GfxRenderer.h>
#include <UnifiedCache.h>  // v2.0.167 — Phase 5 unified streaming cache
#include <Logging.h>
#include <Page.h>
#include <ParsedText.h>
#include <SDCardManager.h>
#include <SharedSpiLock.h>
#include <Utf8.h>
#include <blocks/ImageBlock.h>

// v2.0.117 Phase R2.6 — markerize side-channel for FB2.  See header.  Active
// only when SNAPIX_MARKERIZER=1 (currently only in v3_alpha env).
//
// v2.0.130 R4.c — adds the streaming paginator + adapter so the parser
// can build the `.idx` upfront via a MEASURE-only walk, eliminating
// the legacy parser dependency even on a section's first visit.
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
#include <Arduino.h>          // vTaskDelay (yield between markerize chunks)
#include <FS.h>
#include <Fb2Stripper.h>
#include <GfxRendererPaginatorAdapter.h>
#include <LittleFS.h>
#include <MarkerizedPageRender.h>
#include <MarkerizeChapter.h>
#include <StreamingPaginator.h>
#endif

#define TAG "FB2_PARSE"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {
constexpr size_t READ_CHUNK_SIZE = 2048;

bool isWhitespace(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

int utf8SafePrefixLength(const char* data, const int len, const int maxBytes) {
  const int limit = std::min(len, maxBytes);
  int consumed = 0;

  while (consumed < limit) {
    const unsigned char lead = static_cast<unsigned char>(data[consumed]);
    int cpLen = 1;
    if ((lead & 0x80U) == 0) {
      cpLen = 1;
    } else if ((lead & 0xE0U) == 0xC0U) {
      cpLen = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
      cpLen = 3;
    } else if ((lead & 0xF8U) == 0xF0U) {
      cpLen = 4;
    }

    if (consumed + cpLen > limit) {
      break;
    }
    consumed += cpLen;
  }

  return consumed > 0 ? consumed : limit;
}

const char* stripNamespace(const char* name) {
  const char* local = strrchr(name, ':');
  return local ? local + 1 : name;
}
}  // namespace

Fb2Parser::Fb2Parser(std::string filepath, GfxRenderer& renderer, const RenderConfig& config, const uint32_t startOffset,
                     const int startingSectionIndex, const bool sectionScoped, const uint32_t endOffset)
    : filepath_(std::move(filepath)),
      renderer_(renderer),
      config_(config),
      startOffset_(startOffset),
      endOffset_(endOffset),
      startingSectionIndex_(startingSectionIndex),
      sectionScoped_(sectionScoped) {}

Fb2Parser::~Fb2Parser() { reset(); }

void Fb2Parser::setFb2(const Fb2* fb2) {
  fb2_ = fb2;
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  if (fb2_) {
    markersDir_ = fb2_->getCachePath() + "/markers";
  } else {
    markersDir_.clear();
  }
#endif
}

// =============================================================================
// v2.0.117 Phase R2.6 — markerize side-channel for FB2.  Best-effort, side-
// channel: failures log but don't fail the parser.  Compiled out entirely
// when SNAPIX_MARKERIZER=0.
//
// Lifecycle: called ONCE per Fb2Parser instance from `parsePages` init path
// (guarded by `markerizeAttempted_`).  For section-scoped parsers, markerizes
// just the [startOffset_..endOffset_) byte range.  For full-file parsers,
// markerizes the entire FB2 source from byte 0.
// =============================================================================
bool Fb2Parser::tryMarkerizeSection() {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  if (!fb2_ || filepath_.empty()) return false;

  // v2.0.167 — markers now live as a UnifiedCache::Markers segment in
  // <fb2CachePath>/streaming.cache, keyed by sectionIndex.
  auto cache = snapix::unifiedcache::UnifiedCache::shared(fb2_->getCachePath());
  size_t existingSize = 0;
  if (cache->segmentSize(snapix::unifiedcache::Kind::Markers,
                         static_cast<uint16_t>(startingSectionIndex_), &existingSize)) {
    LOG_INF(TAG, "[CONTENT][FB2] markerize cache hit section=%d (UnifiedCache::Markers size=%zu)",
            startingSectionIndex_, existingSize);
    return true;
  }

  // Open SD source separately from the parser's `file_` so the parser's
  // position cursor isn't disturbed.  Same SPI bus is shared — caller
  // must ensure markerize runs OUTSIDE any SharedBusLock (we open inside
  // our own).
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
  const bool ok = cache->writeSegmentStreamingDeferred(
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

void Fb2Parser::releaseStreamingState() {
  if (xmlParser_) {
    XML_ParserFree(xmlParser_);
    xmlParser_ = nullptr;
  }
  if (file_) {
    snapix::spi::SharedBusLock lk;
    file_.close();
  }
  initialized_ = false;
  suspended_ = false;
  xmlParserSuspended_ = false;
}

void Fb2Parser::reset() {
  releaseStreamingState();
  hasMore_ = true;
  isRtl_ = false;
  stopRequested_ = false;
  depth_ = 0;
  skipUntilDepth_ = INT_MAX;
  boldUntilDepth_ = INT_MAX;
  italicUntilDepth_ = INT_MAX;
  inBody_ = false;
  inTitle_ = false;
  inSubtitle_ = false;
  inParagraph_ = false;
  bodyCount_ = 0;
  sectionCounter_ = startingSectionIndex_;
  firstSection_ = true;
  targetSectionStarted_ = false;
  targetSectionDepth_ = 0;
  fragmentComplete_ = false;
  xmlParserSuspended_ = false;
  pendingNewTextBlock_ = false;
  pendingBlockStyle_ = TextBlock::LEFT_ALIGN;
  pendingSectionStart_ = false;
  pendingSectionNeedsPageBreak_ = false;
  pendingSectionAnchorIndex_ = -1;
  delete[] partWordBuffer_;
  partWordBuffer_ = nullptr;
  partWordBufferIndex_ = 0;
  rtlArabicWords_ = 0;
  rtlLtrWords_ = 0;
  currentTextBlock_.reset();
  currentPage_.reset();
  currentPageNextY_ = 0;
  pagesCreated_ = 0;
  hitMaxPages_ = false;
  fileSize_ = 0;
  lastParsedOffset_ = startOffset_;
  anchorMap_.clear();
  // v2.0.132 — drop any in-flight short-circuit state.
  shortCircuitActive_ = false;
  shortCircuitTotalPages_ = 0;
  shortCircuitNextPage_ = 0;
}

void Fb2Parser::requestXmlSuspend() {
  hitMaxPages_ = true;
  stopRequested_ = true;
  if (xmlParser_ && !xmlParserSuspended_) {
    XML_StopParser(xmlParser_, XML_TRUE);
    xmlParserSuspended_ = true;
  }
}

bool Fb2Parser::finishPendingSectionStart() {
  if (!pendingSectionStart_) {
    return true;
  }

  const bool needsPageBreak = pendingSectionNeedsPageBreak_;
  const int anchorIndex = pendingSectionAnchorIndex_;

  if (needsPageBreak) {
    if (currentPage_ && !currentPage_->elements.empty()) {
      onPageComplete_(std::move(currentPage_));
      pagesCreated_++;
      if (maxPages_ > 0 && pagesCreated_ >= maxPages_) {
        requestXmlSuspend();
        return false;
      }
    }
    startNewPage();
  }

  firstSection_ = false;
  if (anchorIndex >= 0) {
    anchorMap_.emplace_back("section_" + std::to_string(anchorIndex), pagesCreated_);
  }

  pendingSectionStart_ = false;
  pendingSectionNeedsPageBreak_ = false;
  pendingSectionAnchorIndex_ = -1;
  return true;
}

bool Fb2Parser::flushDeferredLayoutBeforeResume() {
  if (currentTextBlock_ && !currentTextBlock_->isEmpty()) {
    makePages();
    if (stopRequested_) {
      suspended_ = true;
      hasMore_ = true;
      return false;
    }
  }

  if (!finishPendingSectionStart()) {
    suspended_ = true;
    hasMore_ = true;
    return false;
  }

  if (pendingNewTextBlock_) {
    pendingNewTextBlock_ = false;
    currentTextBlock_.reset(new ParsedText(pendingBlockStyle_, config_.indentLevel, config_.hyphenation, true, isRtl_));
  }

  return true;
}

bool Fb2Parser::parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages,
                           const AbortCallback& shouldAbort) {
  onPageComplete_ = onPageComplete;
  maxPages_ = maxPages;
  pagesCreated_ = 0;
  hitMaxPages_ = false;
  stopRequested_ = false;
  shouldAbort_ = shouldAbort;

#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  // v2.0.132 — R4.b STATEFUL short-circuit continuation.  See parallel
  // EpubChapterParser::parsePages branch for rationale.  Cache batches
  // with maxPages=1 typically — drain one batch worth of empty pages
  // per call until shortCircuitNextPage_ == shortCircuitTotalPages_.
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
    }
    LOG_INF(TAG,
            "[CONTENT][FB2] [STREAM] short-circuit batch section=%d emitted=%u..%u of %u hasMore=%u",
            startingSectionIndex_, static_cast<unsigned>(startPage),
            static_cast<unsigned>(shortCircuitNextPage_),
            static_cast<unsigned>(shortCircuitTotalPages_), static_cast<unsigned>(hasMore_));
    return pagesCreated_ > 0;
  }
#endif

  if (!canResume()) {
    reset();

    if (!SdMan.openFileForRead("FB2", filepath_, file_)) {
      LOG_ERR(TAG, "Failed to open file: %s", filepath_.c_str());
      return false;
    }

    {
      snapix::spi::SharedBusLock lk;
      fileSize_ = file_.size();
      lastParsedOffset_ = startOffset_;

      if (startOffset_ > 0) {
        file_.seek(startOffset_);
      }
    }

    xmlParser_ = XML_ParserCreate("UTF-8");
    if (!xmlParser_) {
      LOG_ERR(TAG, "Failed to create XML parser");
      releaseStreamingState();
      return false;
    }

    XML_SetUserData(xmlParser_, this);
    XML_SetElementHandler(xmlParser_, startElement, endElement);
    XML_SetCharacterDataHandler(xmlParser_, characterData);

    // v2.0.117 Phase R2.6 — markerize side-channel runs ONCE per Fb2Parser
    // instance (one-shot via markerizeAttempted_), before the legacy Expat
    // pass kicks in.  Best-effort: failure logs but doesn't block the
    // legacy parser.  Compiled out entirely in default env.
    if (!markerizeAttempted_) {
      markerizeAttempted_ = true;
      (void)tryMarkerizeSection();
    }

#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
    // v2.0.130 R4.c — build `.idx` upfront via MEASURE-only walk so
    // the R4.b short-circuit below fires on FIRST visit, not just
    // repeats.  See parallel change in EpubChapterParser::parsePages
    // for the full rationale and cost analysis.
    if (fb2_) {
      // v2.0.167 — markers + idx in UnifiedCache (per book streaming.cache).
      auto ucache = snapix::unifiedcache::UnifiedCache::shared(fb2_->getCachePath());
      do {
        if (ucache->segmentSize(snapix::unifiedcache::Kind::Idx,
                                static_cast<uint16_t>(startingSectionIndex_), nullptr)) {
          break;  // R4.b will handle existing idx
        }
        File mf;
        size_t markersStreamSize = 0;
        if (!ucache->openSegmentReader(snapix::unifiedcache::Kind::Markers,
                                        static_cast<uint16_t>(startingSectionIndex_), mf,
                                        &markersStreamSize)) {
          break;  // markerize hasn't produced markers yet
        }

        // v2.0.131 — paginator config from real viewport margins (set via
        // setStreamingViewport from ReaderStateAsyncJobs).  See parallel
        // change in EpubChapterParser.cpp for the rationale (avoids the
        // configHash mismatch and pageCount mismatch that caused the
        // v2.0.130 white-screen-on-page-turn bug).
        const uint16_t bodyLineH = static_cast<uint16_t>(renderer_.getLineHeight(config_.fontId));
        snapix::smolport::StreamingPaginatorConfig cfg{};
        cfg.pageWidth = static_cast<uint16_t>(renderer_.getScreenWidth());
        cfg.pageHeight = static_cast<uint16_t>(renderer_.getScreenHeight());
        cfg.marginTop = static_cast<uint16_t>(streamingViewportMarginTop_);
        cfg.marginBottom = static_cast<uint16_t>(streamingViewportMarginBottom_);
        cfg.marginLeft = static_cast<uint16_t>(streamingViewportMarginLeft_);
        cfg.marginRight = static_cast<uint16_t>(streamingViewportMarginRight_);
        cfg.bodyLineHeight = bodyLineH > 0 ? bodyLineH : 24;
        cfg.headingLineHeight = static_cast<uint16_t>(cfg.bodyLineHeight * 3 / 2);
        cfg.paragraphSpacing = static_cast<uint16_t>(cfg.bodyLineHeight / 4);

        // v2.0.136 — diagnostic log; mirrors EpubChapterParser.
        LOG_INF(TAG,
                "[CONTENT][FB2] [STREAM] R4.c paginator cfg section=%d fontId=%d "
                "pageW=%u pageH=%u mT=%u mB=%u mL=%u mR=%u bodyLH=%u",
                startingSectionIndex_, config_.fontId,
                static_cast<unsigned>(cfg.pageWidth), static_cast<unsigned>(cfg.pageHeight),
                static_cast<unsigned>(cfg.marginTop), static_cast<unsigned>(cfg.marginBottom),
                static_cast<unsigned>(cfg.marginLeft), static_cast<unsigned>(cfg.marginRight),
                static_cast<unsigned>(cfg.bodyLineHeight));

        // v2.0.145/147 — image resolver for FB2.  Calls Fb2::cacheImage
        // for lazy decode-on-demand so MEASURE walk page boundaries
        // account for image heights even on first visit (cacheImage
        // is idempotent; subsequent calls just re-open the cached BMP).
        const Fb2* fb2Ptr = fb2_;
        const uint16_t imgMaxW =
            static_cast<uint16_t>(cfg.pageWidth - cfg.marginLeft - cfg.marginRight);
        const uint16_t imgMaxH = static_cast<uint16_t>(cfg.pageHeight -
                                                         cfg.marginTop -
                                                         cfg.marginBottom);
        auto resolveImage = [fb2Ptr, imgMaxW, imgMaxH](const uint8_t* p,
                                                        size_t l) -> std::string {
          if (fb2Ptr == nullptr || p == nullptr || l == 0) return {};
          std::string src(reinterpret_cast<const char*>(p), l);
          if (!src.empty() && src[0] == '#') src.erase(0, 1);
          std::string outPath;
          uint16_t w = 0, h = 0;
          const bool ok = fb2Ptr->cacheImage(src, outPath, w, h, imgMaxW, imgMaxH,
                                              /*fastMode=*/true, /*shouldAbort=*/{});
          return ok ? outPath : std::string();
        };
        // v2.0.146 — pass fakeBold for layout consistency between
        // MEASURE-walk and runtime render.  See EpubChapterParser.cpp.
        snapix::smolport::GfxRendererPaginatorAdapter adapter(renderer_, config_.fontId,
                                                                config_.fontId, true,
                                                                resolveImage,
                                                                config_.fakeBold);
        snapix::smolport::StreamingPaginator paginator(cfg, adapter);

        constexpr size_t kChunkBufBytes = 4096;
        uint8_t chunkBuf[kChunkBufBytes];
        // v2.0.167 — markers reader is in streaming.cache; bound reads.
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

        // v2.0.175 — same short-section fix as EpubChapterParser.cpp.  FB2
        // sections sometimes contain only a brief title or a single
        // paragraph (especially title-only sections, dedications, or
        // sub-chapter dividers) whose markers fit in less than one page.
        // The paginator never fires an overflow-triggered boundary
        // callback for such sections, leaving `captured` empty and the
        // R4.c block breaking without writing an idx — which propagates
        // up as a hard "parsePages returned no output" failure that
        // ReaderState retries forever.  Synthesize a single page-0
        // boundary in that case so R4.b emits one Page and the streaming
        // render path shows the short content correctly.
        if (captured.empty()) {
          if (stats.bytesConsumed == 0) {
            LOG_ERR(TAG,
                    "[CONTENT][FB2] [STREAM] R4.c read 0 bytes from markers section=%d "
                    "size=%zu — markers segment is empty/corrupt",
                    startingSectionIndex_, markersStreamSize);
            break;
          }
          LOG_INF(TAG,
                  "[CONTENT][FB2] [STREAM] R4.c short section=%d: synthesizing 1-page "
                  "idx (markers=%zu bytes consumed=%u, fits in less than one page)",
                  startingSectionIndex_, markersStreamSize, static_cast<unsigned>(stats.bytesConsumed));
          snapix::smolport::PageBoundarySnapshot s{};
          s.pageIndex = 0;
          s.byteOffset = 0;
          s.styleBits = 0;
          captured.push_back(s);
        }

        const uint16_t configHash =
            snapix::smolport::computePageIndexConfigHash(cfg, config_.fontId,
                                                          config_.fakeBold);
        const size_t needed = snapix::smolport::kPageIndexHeaderBytes +
                               captured.size() * snapix::smolport::kPageIndexEntryBytes;
        if (needed > 4096) break;
        uint8_t serdebuf[4096];
        const size_t wrote = snapix::smolport::serializePageIndex(
            captured.data(), captured.size(), configHash, serdebuf, sizeof(serdebuf));
        if (wrote == 0) break;

        // v2.0.167 — write idx as UnifiedCache::Idx segment.
        if (!ucache->writeSegment(snapix::unifiedcache::Kind::Idx,
                                  static_cast<uint16_t>(startingSectionIndex_), serdebuf, wrote)) {
          LOG_ERR(TAG, "[CONTENT][FB2] [STREAM] idx write to UnifiedCache failed section=%d",
                  startingSectionIndex_);
          break;
        }
        LOG_INF(TAG,
                "[CONTENT][FB2] [STREAM] idx built upfront section=%d pages=%u bytes=%u (skipping legacy parser)",
                startingSectionIndex_, static_cast<unsigned>(captured.size()),
                static_cast<unsigned>(stats.bytesConsumed));
      } while (false);
    }
#endif

    // v2.0.129 R4.b — short-circuit: if `.idx` sidecar exists, emit
    // that many empty Page objects and skip the legacy Expat parser
    // entirely.  See parallel change in EpubChapterParser::parsePages
    // for the rationale (eliminates Page-tree heap allocs that drive
    // the cold-extend OOM class of failures on memory-tight chapters).
    // Falls through to legacy parser if `.idx` missing/malformed.
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
    if (fb2_) {
      // v2.0.167 — read idx from UnifiedCache::Idx segment.
      auto ucache = snapix::unifiedcache::UnifiedCache::shared(fb2_->getCachePath());
      do {
        File idxF;
        size_t idxSegSize = 0;
        if (!ucache->openSegmentReader(snapix::unifiedcache::Kind::Idx,
                                        static_cast<uint16_t>(startingSectionIndex_), idxF,
                                        &idxSegSize)) {
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
        // Compare against the canonical constant — see parallel comment in
        // EpubChapterParser.cpp.  v2.0.134 bumped 1→2 to invalidate pre-fix
        // paginator's offsets.
        const uint16_t version = static_cast<uint16_t>(header[4]) |
                                  (static_cast<uint16_t>(header[5]) << 8);
        if (version != snapix::smolport::kPageIndexVersion) break;
        const uint16_t pageCount = static_cast<uint16_t>(header[8]) |
                                    (static_cast<uint16_t>(header[9]) << 8);
        if (pageCount == 0) break;

        LOG_INF(TAG,
                "[CONTENT][FB2] [STREAM] short-circuit: total %u pages from .idx section=%d "
                "(skipping legacy parser)",
                static_cast<unsigned>(pageCount), startingSectionIndex_);

        // v2.0.132 — activate STATEFUL short-circuit so cache's batched
        // extend() calls drain the .idx page-count incrementally.  Pre-
        // fix, a maxPages=1 first call emitted 1 of N pages and set
        // hasMore_=false, claiming the section was 1 page total — user
        // couldn't navigate past page 0 of ANY FB2 section.
        shortCircuitActive_ = true;
        shortCircuitTotalPages_ = pageCount;
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
          releaseStreamingState();
          suspended_ = false;
        }
        // Else: keep shortCircuitActive_=true; canResume() returns true
        // until the cache drains the remaining pages.

        LOG_INF(TAG,
                "[CONTENT][FB2] [STREAM] short-circuit batch section=%d pagesCreated=%u nextPage=%u "
                "total=%u hasMore=%u",
                startingSectionIndex_, static_cast<unsigned>(pagesCreated_),
                static_cast<unsigned>(shortCircuitNextPage_),
                static_cast<unsigned>(shortCircuitTotalPages_), static_cast<unsigned>(hasMore_));

        // Close the file we opened above — we never started parsing.
        // (Subsequent shortCircuitActive_ batches re-enter through the
        // top-of-parsePages branch and don't touch file_ at all.)
        {
          snapix::spi::SharedBusLock lk;
          if (file_) file_.close();
        }
        return pagesCreated_ > 0;
      } while (false);
    }
#endif

    startNewPage();
    if (startOffset_ > 0) {
      constexpr char kSyntheticPrefix[] = "<FictionBook><body>";
      if (XML_Parse(xmlParser_, kSyntheticPrefix, static_cast<int>(sizeof(kSyntheticPrefix) - 1), 0) ==
          XML_STATUS_ERROR) {
        LOG_ERR(TAG, "Failed to initialize section parser");
        releaseStreamingState();
        return false;
      }
    }
    initialized_ = true;
  } else {
    suspended_ = false;

    // A previous batch may have stopped while laying out an already parsed
    // paragraph.  Finish that tail before Expat is resumed, otherwise new XML
    // characterData can be appended to the old ParsedText and shift/duplicate
    // page-boundary text.
    if (!flushDeferredLayoutBeforeResume()) {
      return true;
    }

    // The Expat parser may have been suspended mid-buffer via
    // XML_StopParser(resumable) when hitMaxPages fired.  Finish
    // processing the remainder of that buffer before reading new data.
    if (xmlParserSuspended_) {
      xmlParserSuspended_ = false;
      const XML_Status rs = XML_ResumeParser(xmlParser_);
      if (rs == XML_STATUS_ERROR) {
        const XML_Error ec = XML_GetErrorCode(xmlParser_);
        if (!(fragmentComplete_ && ec == XML_ERROR_ABORTED)) {
          LOG_ERR(TAG, "Resume parse error at line %lu: %s",
                  XML_GetCurrentLineNumber(xmlParser_), XML_ErrorString(ec));
          releaseStreamingState();
          currentTextBlock_.reset();
          currentPage_.reset();
          partWordBufferIndex_ = 0;
          return false;
        }
        // fragmentComplete_ during resume — fall through to flush below
      } else if (stopRequested_) {
        // hitMaxPages fired again during resumed processing
        suspended_ = true;
        hasMore_ = true;
        return true;
      }
    }
  }

  // If the section was fully parsed during the resumed buffer,
  // skip the main read loop and go straight to flush/finalize.
  if (!fragmentComplete_) {
    // Single buffer reused for parsing (saves stack)
    uint8_t buffer[READ_CHUNK_SIZE + 1];
    uint16_t abortCheckCounter = 0;

    while (true) {
      // --- SPI-locked section: read a chunk from SD card ---
      int bytesRead;
      int done;
      {
        snapix::spi::SharedBusLock lk;
        if (file_.available() <= 0) break;

        size_t bytesToRead = READ_CHUNK_SIZE;
        if (sectionScoped_ && endOffset_ > startOffset_) {
          const size_t pos = file_.position();
          if (pos >= endOffset_) {
            fragmentComplete_ = true;
            break;
          }
          bytesToRead = std::min(bytesToRead, static_cast<size_t>(endOffset_ - pos));
        }

        bytesRead = file_.read(buffer, bytesToRead);
        if (bytesRead <= 0) {
          if (bytesRead < 0) {
            LOG_ERR(TAG, "SD read error at offset %lu", static_cast<unsigned long>(file_.position()));
          }
          break;
        }
        done = (file_.available() == 0 && !(sectionScoped_ && endOffset_ > startOffset_)) ? 1 : 0;
      }
      // --- SPI released: display driver can use the bus while we process XML ---

      if (shouldAbort_ && (++abortCheckCounter % 10 == 0) && shouldAbort_()) {
        // Suspend gracefully instead of tearing down state: the abort fires
        // BETWEEN XML_Parse() calls, so Expat is in a clean state and the
        // file cursor matches lastParsedOffset_.  Keeping xmlParser_ + file_
        // alive lets canResume() return true on the next parsePages() call,
        // so the BG worker resumes hot-extend instead of cold-rebuilding the
        // entire section from byte 0 each preempt.  PageCache periodically
        // forces a cold reset when heap defragments below 15 KB largest free
        // block (PageCache.cpp:933) — that keeps the long-running parser
        // from permanently pinning fragmented memory.
        LOG_INF(TAG, "Aborted by external request — parser suspended for resume");
        suspended_ = true;
        hasMore_ = true;
        return false;
      }

      if (XML_Parse(xmlParser_, reinterpret_cast<const char*>(buffer), bytesRead, done) ==
          XML_STATUS_ERROR) {
        if (!(fragmentComplete_ && XML_GetErrorCode(xmlParser_) == XML_ERROR_ABORTED)) {
          LOG_ERR(TAG, "Parse error at line %lu: %s", XML_GetCurrentLineNumber(xmlParser_),
                  XML_ErrorString(XML_GetErrorCode(xmlParser_)));
          releaseStreamingState();
          currentTextBlock_.reset();
          currentPage_.reset();
          partWordBufferIndex_ = 0;
          return false;
        }
        break;
      }

      {
        snapix::spi::SharedBusLock lk;
        lastParsedOffset_ = static_cast<uint32_t>(std::min<size_t>(file_.position(), fileSize_));
        if (sectionScoped_ && endOffset_ > startOffset_ && file_.position() >= endOffset_) {
          fragmentComplete_ = true;
        }
      }

      if (stopRequested_) {
        suspended_ = true;
        hasMore_ = true;
        return true;
      }

      if (fragmentComplete_) {
        break;
      }
    }
  }

  // Flush remaining content
  flushPartWordBuffer();
  if (currentTextBlock_ && !currentTextBlock_->isEmpty()) {
    makePages();
    if (stopRequested_) {
      suspended_ = true;
      hasMore_ = true;
      return true;
    }
  }

  // Emit final page
  if (currentPage_ && !currentPage_->elements.empty()) {
    onPageComplete_(std::move(currentPage_));
    pagesCreated_++;
  }

  releaseStreamingState();
  currentTextBlock_.reset();
  currentPage_.reset();
  hasMore_ = false;

  LOG_INF(TAG, "Parsed %d pages from %s", pagesCreated_, filepath_.c_str());
  return true;
}

// v2.0.63 had try/catch wrappers in each expat callback to absorb
// std::bad_alloc.  Removed in v2.0.64 — the wrapper turned out to be
// purely cosmetic on a truly OOM heap: throwing std::bad_alloc itself
// requires __cxa_allocate_exception to malloc ~16 B for the exception
// object, and that allocation also fails on a fragmented heap.  The
// C++ runtime then calls __terminate() → abort(), bypassing any
// catch block we wrote.  The right fix is to NOT enter parser code
// paths when heap headroom is insufficient — see the pre-flight check
// in PageCache::extend (cold path) which gates rebuild on
// largest>=25K & free>=50K.  Combined with ParsedText::words pre-
// reservation (64 entries) the parser doesn't trigger mid-parse
// vector growth that would have OOM'd.

void XMLCALL Fb2Parser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<Fb2Parser*>(userData);
  const char* localName = stripNamespace(name);

  // Prevent stack overflow from deeply nested XML
  if (self->depth_ >= 100) {
    self->depth_++;
    return;
  }

  if (self->skipUntilDepth_ < self->depth_) {
    self->depth_++;
    return;
  }

  if (strcmp(localName, "binary") == 0) {
    self->skipUntilDepth_ = self->depth_;
    self->depth_++;
    return;
  }

  if (strcmp(localName, "body") == 0) {
    self->bodyCount_++;
    self->inBody_ = (self->bodyCount_ == 1);
    self->depth_++;
    return;
  }

  if (!self->inBody_) {
    self->depth_++;
    return;
  }

  if (strcmp(localName, "section") == 0) {
    const bool needsPageBreak = !self->firstSection_;
    self->sectionCounter_++;
    const int sectionAnchorIndex = self->sectionCounter_ - 1;
    if (self->sectionScoped_) {
      self->targetSectionStarted_ = true;
      self->targetSectionDepth_++;
    }
    if (needsPageBreak) {
      // Flush current content before new section
      self->flushPartWordBuffer();
      if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
        self->makePages();
        if (self->stopRequested_) {
          self->pendingSectionStart_ = true;
          self->pendingSectionNeedsPageBreak_ = true;
          self->pendingSectionAnchorIndex_ = sectionAnchorIndex;
          self->depth_++;
          return;
        }
      }
    }

    self->pendingSectionStart_ = true;
    self->pendingSectionNeedsPageBreak_ = needsPageBreak;
    self->pendingSectionAnchorIndex_ = sectionAnchorIndex;
    if (!self->finishPendingSectionStart()) {
      self->depth_++;
      return;
    }
  } else if (strcmp(localName, "title") == 0) {
    self->inTitle_ = true;
    self->boldUntilDepth_ = std::min(self->boldUntilDepth_, self->depth_);
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->startNewTextBlock(TextBlock::CENTER_ALIGN);
  } else if (strcmp(localName, "subtitle") == 0) {
    self->inSubtitle_ = true;
    self->boldUntilDepth_ = std::min(self->boldUntilDepth_, self->depth_);
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->startNewTextBlock(TextBlock::CENTER_ALIGN);
  } else if (strcmp(localName, "p") == 0) {
    self->inParagraph_ = true;
    if (!self->currentTextBlock_) {
      TextBlock::BLOCK_STYLE style = self->inTitle_ || self->inSubtitle_
                                         ? TextBlock::CENTER_ALIGN
                                         : static_cast<TextBlock::BLOCK_STYLE>(self->config_.paragraphAlignment);
      self->startNewTextBlock(style);
    }
  } else if (strcmp(localName, "emphasis") == 0) {
    // Flush any partial word collected under the *outer* (non-italic) style
    // before italic takes effect.  Otherwise text like "abc<emphasis>def"
    // would emit "abcdef" attributed to whichever style is active at the
    // next flush boundary.
    if (self->partWordBufferIndex_ > 0) {
      self->flushPartWordBuffer();
    }
    self->italicUntilDepth_ = std::min(self->italicUntilDepth_, self->depth_);
  } else if (strcmp(localName, "strong") == 0) {
    if (self->partWordBufferIndex_ > 0) {
      self->flushPartWordBuffer();
    }
    self->boldUntilDepth_ = std::min(self->boldUntilDepth_, self->depth_);
  } else if (strcmp(localName, "empty-line") == 0) {
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
      if (self->stopRequested_) {
        self->depth_++;
        return;
      }
    }
    self->addVerticalSpacing(1);
  } else if (strcmp(localName, "image") == 0) {
    // Inline FB2 image (<image l:href="#binary_id"/>).  When an Fb2 instance
    // is wired (setFb2) and showImages is on, materialise the referenced
    // <binary> base64 block into a cached BMP and emit it as a PageImage.
    // Otherwise silently skip — keeps legacy behaviour intact.
    if (!self->fb2_ || !self->config_.showImages || self->stopRequested_) return;

    const char* href = nullptr;
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        const char* an = atts[i];
        const char* p = strrchr(an, ':');
        p = p ? (p + 1) : an;
        if (strcmp(p, "href") == 0) {
          href = atts[i + 1];
          break;
        }
      }
    }
    if (!href || href[0] != '#' || !href[1]) return;
    std::string binaryId(href + 1);

    // Cap output dimensions to the viewport so the JPEG is downscaled by the
    // converter rather than blown up at render time.  Leave room for at
    // least a couple of text lines around the image.
    const int maxW = std::max(64, static_cast<int>(self->config_.viewportWidth) - 12);
    const int maxH = std::max(64, static_cast<int>(self->config_.viewportHeight) - 80);

    std::string bmpPath;
    uint16_t w = 0, h = 0;
    // Fast mode: registers the binary, peeks header for dims, but defers the
    // (slow) JPEG / PNG → BMP pixel decode to a BG worker.  Pagination needs
    // accurate dims here, so we still pay the header-peek cost; decoded BMP
    // appears later via Fb2::decodePendingImages() and ImageBlock::render
    // shows a placeholder ("[Image]") until then.  The trade-off keeps the
    // page-turn under the user's perceptual threshold instead of stalling
    // for 3-8 s per image.
    if (!self->fb2_->cacheImage(binaryId, bmpPath, w, h, maxW, maxH, /*fastMode*/ true, self->shouldAbort_) ||
        w == 0 || h == 0) {
      LOG_DBG(TAG, "image <%s>: cache miss, falling back to skip", binaryId.c_str());
      return;
    }

    auto imageBlock = std::make_shared<ImageBlock>(bmpPath, w, h, /*nodeId*/ "", binaryId, /*resolved*/ "");

    // Flush the current text run so the image lands on its own paragraph.
    // Note: makePages() may call requestXmlSuspend() if it commits the
    // page that hits the per-batch maxPages limit, which sets
    // stopRequested_=true.  We must NOT bail out here on stopRequested_
    // — that would drop `imageBlock` on the floor.  When this happens the
    // user's just-decoded placeholder ImageBlock would never reach the
    // serialised page; the next page-render shows ONLY text and the
    // figure's caption awkwardly slides to the next page (as the user
    // observed for "рисунок 10" / i_019.jpg in the v2.0.47 trace).
    // Instead, ALWAYS add the image to whatever page is current after
    // the flush, then let the outer parser loop handle the suspend.
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->addImageToPage(std::move(imageBlock));
  }

  self->depth_++;
}

void XMLCALL Fb2Parser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<Fb2Parser*>(userData);
  const char* localName = stripNamespace(name);

  self->depth_--;

  // If the closing tag is about to turn bold or italic OFF, flush any partial
  // word still in the buffer *before* clearing the style anchor — otherwise
  // the last word inside `<strong>...</strong>` (with no trailing whitespace,
  // e.g. when followed immediately by `</p>`) gets emitted under the post-tag
  // style.  This is what makes the last bold word in a line render as regular
  // under fakeBold.
  const bool willClearBold = self->depth_ <= self->boldUntilDepth_;
  const bool willClearItalic = self->depth_ <= self->italicUntilDepth_;
  if ((willClearBold || willClearItalic) && self->partWordBufferIndex_ > 0) {
    self->flushPartWordBuffer();
  }

  if (willClearBold) {
    self->boldUntilDepth_ = INT_MAX;
  }
  if (willClearItalic) {
    self->italicUntilDepth_ = INT_MAX;
  }

  if (self->skipUntilDepth_ == self->depth_) {
    self->skipUntilDepth_ = INT_MAX;
    return;
  }

  if (!self->inBody_) {
    if (strcmp(localName, "body") == 0) {
      // Closing body tag — nothing more to do
    }
    return;
  }

  if (strcmp(localName, "body") == 0) {
    self->inBody_ = false;
    return;
  }

  if (strcmp(localName, "title") == 0) {
    self->inTitle_ = false;
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
      if (self->stopRequested_) {
        return;
      }
    }
    self->addVerticalSpacing(1);
  } else if (strcmp(localName, "subtitle") == 0) {
    self->inSubtitle_ = false;
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
      if (self->stopRequested_) {
        return;
      }
    }
    self->addVerticalSpacing(1);
  } else if (strcmp(localName, "p") == 0) {
    self->inParagraph_ = false;
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
      if (self->stopRequested_) {
        return;
      }
    }
  } else if (strcmp(localName, "section") == 0 && self->sectionScoped_ && self->targetSectionStarted_) {
    if (self->targetSectionDepth_ > 0) {
      self->targetSectionDepth_--;
    }
    if (self->targetSectionDepth_ == 0 && self->xmlParser_) {
      self->fragmentComplete_ = true;
      XML_StopParser(self->xmlParser_, XML_FALSE);
    }
  }
}

void XMLCALL Fb2Parser::characterData(void* userData, const XML_Char* s, int len) {
  auto* self = static_cast<Fb2Parser*>(userData);
  if (self->skipUntilDepth_ < self->depth_) return;
  if (!self->inBody_) return;

  int offset = 0;
  while (offset < len) {
    while (offset < len && isWhitespace(s[offset])) {
      if (self->partWordBufferIndex_ > 0) {
        self->flushPartWordBuffer();
      }
      offset++;
    }

    const int runStart = offset;
    while (offset < len && !isWhitespace(s[offset])) {
      offset++;
    }

    if (offset > runStart) {
      self->appendPartWordBytes(s + runStart, offset - runStart);
    }
  }
}

void Fb2Parser::appendPartWordBytes(const char* data, int len) {
  if (!partWordBuffer_) {
    partWordBuffer_ = new char[MAX_WORD_SIZE + 1];
    partWordBufferIndex_ = 0;
  }
  int remaining = len;
  const char* src = data;

  while (remaining > 0) {
    if (partWordBufferIndex_ >= MAX_WORD_SIZE) {
      flushPartWordBuffer();
    }

    const int spaceLeft = MAX_WORD_SIZE - partWordBufferIndex_;
    const int chunkLen = utf8SafePrefixLength(src, remaining, spaceLeft);
    memcpy(partWordBuffer_ + partWordBufferIndex_, src, chunkLen);
    partWordBufferIndex_ += chunkLen;
    src += chunkLen;
    remaining -= chunkLen;

    if (partWordBufferIndex_ >= MAX_WORD_SIZE) {
      flushPartWordBuffer();
    }
  }
}

void Fb2Parser::flushPartWordBuffer() {
  if (!currentTextBlock_ || partWordBufferIndex_ == 0) {
    partWordBufferIndex_ = 0;
    return;
  }

  partWordBuffer_[partWordBufferIndex_] = '\0';
  partWordBufferIndex_ = static_cast<int>(utf8NormalizeNfc(partWordBuffer_, partWordBufferIndex_));
  observeTextDirectionSample(partWordBuffer_);
  refreshTextDirection();
  currentTextBlock_->addWord(partWordBuffer_, getCurrentFontFamily());
  partWordBufferIndex_ = 0;
}

void Fb2Parser::observeTextDirectionSample(const char* word) {
  if (!word || !*word) {
    return;
  }

  switch (ScriptDetector::classify(word)) {
    case ScriptDetector::Script::ARABIC:
      if (rtlArabicWords_ < UINT16_MAX) {
        rtlArabicWords_++;
      }
      break;
    case ScriptDetector::Script::LATIN:
      if (rtlLtrWords_ < UINT16_MAX) {
        rtlLtrWords_++;
      }
      break;
    default:
      break;
  }
}

void Fb2Parser::refreshTextDirection() {
  const uint16_t strongWordCount = rtlArabicWords_ + rtlLtrWords_;
  if (strongWordCount < 4 && !(rtlArabicWords_ >= 2 && rtlLtrWords_ == 0)) {
    return;
  }

  isRtl_ = rtlArabicWords_ > rtlLtrWords_;
  if (currentTextBlock_) {
    currentTextBlock_->setRtl(isRtl_);
  }
}

void Fb2Parser::startNewTextBlock(TextBlock::BLOCK_STYLE style) {
  if (stopRequested_) {
    pendingNewTextBlock_ = true;
    pendingBlockStyle_ = style;
    return;
  }

  if (currentTextBlock_) {
    if (currentTextBlock_->isEmpty()) {
      currentTextBlock_->setStyle(style);
      return;
    }
    makePages();
    if (stopRequested_) {
      pendingNewTextBlock_ = true;
      pendingBlockStyle_ = style;
      return;
    }
  }
  currentTextBlock_.reset(new ParsedText(style, config_.indentLevel, config_.hyphenation, true, isRtl_));
}

void Fb2Parser::makePages() {
  if (!currentTextBlock_ || currentTextBlock_->isEmpty()) return;

  flushPartWordBuffer();
  refreshTextDirection();

  if (!currentPage_) {
    startNewPage();
  }

  const int lineHeight = static_cast<int>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression);
  bool continueProcessing = true;

  currentTextBlock_->layoutAndExtractLines(
      renderer_, config_.fontId, config_.viewportWidth,
      [this, &continueProcessing](const std::shared_ptr<TextBlock>& line) {
        if (!continueProcessing) return;
        addLineToPage(line);
        if (hitMaxPages_) {
          continueProcessing = false;
        }
      },
      true, [&continueProcessing]() -> bool { return !continueProcessing; });

  // Paragraph spacing (same pattern as PlainTextParser/ChapterHtmlSlimParser)
  if (!hitMaxPages_) {
    switch (config_.spacingLevel) {
      case 1:
        currentPageNextY_ += lineHeight / 4;
        break;
      case 3:
        currentPageNextY_ += lineHeight;
        break;
    }
    currentTextBlock_.reset();
  }
  // else: currentTextBlock_ still has unconsumed words — preserve for next batch
}

void Fb2Parser::addLineToPage(std::shared_ptr<TextBlock> line) {
  if (stopRequested_) {
    return;
  }

  const int lineHeight = static_cast<int>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression);

  if (!currentPage_) {
    startNewPage();
  }

  if (currentPageNextY_ + lineHeight > config_.viewportHeight) {
    onPageComplete_(std::move(currentPage_));
    pagesCreated_++;
    startNewPage();

    if (maxPages_ > 0 && pagesCreated_ >= maxPages_) {
      requestXmlSuspend();
    }
  }

  currentPage_->elements.push_back(std::make_unique<PageLine>(std::move(line), 0, currentPageNextY_));
  currentPageNextY_ += lineHeight;
}

void Fb2Parser::addImageToPage(std::shared_ptr<ImageBlock> image) {
  if (!image || stopRequested_) return;

  const int imageHeight = image->getHeight();
  const int imageWidth = image->getWidth();
  if (imageHeight <= 0 || imageWidth <= 0) return;

  if (!currentPage_) {
    startNewPage();
  }

  // Keep image and its caption ("Рис. N") together on the same page.  FB2
  // typically follows <image/> with a captioning <p>caption</p>; without
  // reserving one extra line below the image, the caption flows to the next
  // page when the image lands at the bottom — visually orphaned.  Reserving
  // ~2 lines means we occasionally push a tall image to the next page even
  // when no caption follows (~1 wasted line) — acceptable trade for the much
  // more jarring caption/image split.  The trailing breathing-room line is
  // already added below after layout, so reserve the caption *line height*
  // here as the lookahead.
  const int captionLine = std::max(8, static_cast<int>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression));
  const int reserveBelow = captionLine;  // 1 line for "Рис. N", caption line itself + breathing line is added after place.

  // If the image won't fit in the remaining space on the current page (with
  // caption reserve), complete the current page first.  This avoids both
  // image clipping AND orphaned captions.
  //
  // v2.0.58 bug context: this branch USED to bail with `return` when
  // committing the current page bumped pagesCreated_ over the per-batch
  // maxPages limit.  But by the time control reaches here, the XML parser
  // has already consumed the <image> element — bailing without placing
  // means the image is dropped from the page layout permanently.  The
  // serialised cache page contains only text; the user observes "image
  // never renders, no placeholder, caption flows to next page" exactly
  // (i_029 / "Таблица 12" pathology).  Fix: ALWAYS place the image, then
  // suspend afterwards if the page budget is exceeded.  The in-flight
  // currentPage_ holding the image survives across XML suspend/resume,
  // and the next batch's first commit (or end-of-file flush) emits it.
  if (currentPageNextY_ + imageHeight + reserveBelow > config_.viewportHeight) {
    if (currentPage_ && !currentPage_->elements.empty()) {
      onPageComplete_(std::move(currentPage_));
      pagesCreated_++;
    }
    startNewPage();
  }

  // Centre the image horizontally within the viewport.  When the cached BMP
  // is wider than the viewport (shouldn't happen given cacheImage's downscale
  // but defensive), clamp xPos to 0.
  int xPos = (static_cast<int>(config_.viewportWidth) - imageWidth) / 2;
  if (xPos < 0) xPos = 0;

  const int yPos = currentPageNextY_;
  currentPage_->elements.push_back(std::make_unique<PageImage>(std::move(image), xPos, yPos));

  // Add a single line height of breathing room below the image so the next
  // paragraph doesn't bump into it.
  const int lineHeight = std::max(8, static_cast<int>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression));
  currentPageNextY_ = static_cast<int16_t>(std::min(yPos + imageHeight + lineHeight, 32767));

  // Image is placed; safe to suspend now if the per-batch budget was hit
  // when we committed the previous page above.  The currentPage_ carrying
  // this image persists across the suspend/resume boundary.
  if (maxPages_ > 0 && pagesCreated_ >= maxPages_) {
    hitMaxPages_ = true;
    requestXmlSuspend();
  }
}

void Fb2Parser::startNewPage() {
  currentPage_.reset(new Page());
  currentPageNextY_ = 0;
}

EpdFontFamily::Style Fb2Parser::getCurrentFontFamily() const {
  bool bold = (boldUntilDepth_ < INT_MAX);
  bool italic = (italicUntilDepth_ < INT_MAX);
  if (bold && italic) return EpdFontFamily::BOLD_ITALIC;
  if (bold) return EpdFontFamily::BOLD;
  if (italic) return EpdFontFamily::ITALIC;
  return EpdFontFamily::REGULAR;
}

void Fb2Parser::addVerticalSpacing(int lines) {
  const int lineHeight = static_cast<int>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression);
  currentPageNextY_ += lineHeight * lines;
}
