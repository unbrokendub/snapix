#pragma once

#include <ContentParser.h>
#include <RenderConfig.h>
#include <StreamingSection.h>  // ChunkedIdxState + extend/loadChunkedSectionIdx (v3.10 no-TOC lazy)

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class Page;
class GfxRenderer;
class Fb2;
namespace snapix::unifiedcache {
class UnifiedCache;
}

/**
 * Content parser for FB2 sections.
 *
 * v3.8.0 — streaming-only (legacy Expat/ParsedText path removed).  Mirrors
 * EpubChapterParser:
 *   tryMarkerizeSection()  (FB2 byte range → Fb2-mode HtmlStripper → markers)
 *   → R4.c upfront idx build (MEASURE walk, with FB2 inline-image heights)
 *   → R4.b short-circuit emits empty Page objects from the idx page count.
 *
 * Two shapes, same code path:
 *   * Section-scoped (TOC present): markerize [startOffset_..endOffset_),
 *     key = sectionIndex.
 *   * Whole-book (no TOC): markerize the whole file from byte 0, key = 0 —
 *     the document is one streamed section, exactly like TXT/MD.
 *
 * ReaderState::renderPageContents reads the markers directly for layout +
 * drawing.  In a build without SNAPIX_MARKERIZER (build-test only) parsePages()
 * produces no pages.
 */
class Fb2Parser : public ContentParser {
 public:
  Fb2Parser(std::string filepath, GfxRenderer& renderer, const RenderConfig& config, uint32_t startOffset = 0,
            int startingSectionIndex = 0, bool sectionScoped = false, uint32_t endOffset = 0);
  ~Fb2Parser() override;

  bool parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages = 0,
                  const AbortCallback& shouldAbort = nullptr) override;
  bool hasMoreContent() const override { return hasMore_; }
  bool canResume() const override {
    // No-TOC (whole-book) uses the progressive/lazy engine; section-scoped (TOC)
    // uses the per-section short-circuit.
    if (!sectionScoped_) {
      return progressiveInit_ && (progEmitCursor_ < progPagesAvailable_ || !sourceExhausted_);
    }
    return shortCircuitActive_ && shortCircuitNextPage_ < shortCircuitTotalPages_;
  }
  void reset() override;

  // Wire an Fb2 instance for cache-path resolution + inline <image> decode
  // during the MEASURE walk (fb2->cacheImage()).  Required for markerize/idx.
  void setFb2(const Fb2* fb2) { fb2_ = fb2; }

  // Plumb real reader viewport margins (see EpubChapterParser) so the R4.c
  // MEASURE walk's paginator config matches the render path.
  void setStreamingViewport(int marginTop, int marginBottom, int marginLeft, int marginRight) {
    streamingViewportMarginTop_ = marginTop;
    streamingViewportMarginBottom_ = marginBottom;
    streamingViewportMarginLeft_ = marginLeft;
    streamingViewportMarginRight_ = marginRight;
  }

 private:
  std::string filepath_;
  GfxRenderer& renderer_;
  RenderConfig config_;
  uint32_t startOffset_ = 0;
  uint32_t endOffset_ = 0;
  int startingSectionIndex_ = 0;
  bool sectionScoped_ = false;
  bool hasMore_ = true;

  const Fb2* fb2_ = nullptr;
  bool markerizeAttempted_ = false;  // one-shot per instance

  int streamingViewportMarginTop_ = 4;
  int streamingViewportMarginBottom_ = 4;
  int streamingViewportMarginLeft_ = 4;
  int streamingViewportMarginRight_ = 4;

  // R4.b stateful short-circuit (mirrors EpubChapterParser).
  bool shortCircuitActive_ = false;
  uint16_t shortCircuitTotalPages_ = 0;
  uint16_t shortCircuitNextPage_ = 0;

  // Callback state for the short-circuit emit.
  std::function<void(std::unique_ptr<Page>)> onPageComplete_;
  uint16_t maxPages_ = 0;
  uint16_t pagesCreated_ = 0;

  // markerize the section's byte range into UnifiedCache::Markers (key =
  // startingSectionIndex_).  One-shot; cache-hit short-circuits.  Used by the
  // section-scoped (TOC) path.
  bool tryMarkerizeSection(snapix::unifiedcache::UnifiedCache& cache);

  // ---- v3.10 — no-TOC (whole-book) LAZY/progressive markerize ----------------
  // A big no-TOC FB2 used to markerize the whole file before page 0.  Now the
  // body is markerized in ~32 KB element-bounded CHUNKS (Markers keys 0,1,2,…),
  // one per parsePages call, with the idx extended incrementally over a
  // ChunkedMarkersReader — mirrors PlainTextParser.  Page 0 renders after chunk
  // 0; image-heavy FB2 also skips reading the trailing <binary> blobs upfront.
  // These members/methods are inert for the section-scoped (TOC) path.
  bool progressiveInit_ = false;
  uint32_t fileSize_ = 0;        // markerize end (whole file for no-TOC)
  uint32_t chunkStart_ = 0;      // source offset where the next chunk starts
  int chunkIdx_ = 0;             // next Markers chunk key
  bool sourceExhausted_ = false; // last chunk ran to fileSize_
  snapix::pagecache::ChunkedIdxState progIdx_;  // boundaries measured so far
  uint16_t progPagesAvailable_ = 0;  // pages the idx currently covers
  uint16_t progEmitCursor_ = 0;      // pages emitted so far

  bool parsePagesProgressive(const std::function<void(std::unique_ptr<Page>)>& onPageComplete,
                             uint16_t maxPages);
  bool ensureProgressiveInit(snapix::unifiedcache::UnifiedCache& cache);
  bool markerizeNextChunk(snapix::unifiedcache::UnifiedCache& cache);
};
