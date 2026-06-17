#pragma once

#include <RenderConfig.h>
#include <SdFat.h>

#include <cstdint>
#include <string>

#include "ContentParser.h"

class GfxRenderer;
class Page;

/**
 * Content parser for plain-text files (.txt).
 *
 * v3.7.0 — migrated from the legacy ParsedText block path to the v3 streaming
 * pipeline (same as EPUB/FB2):
 *   tryMarkerize()  (file → TxtStripper → UnifiedCache::Markers key 0)
 *   → ensureStreamingSectionIdx()  (MEASURE-only walk → UnifiedCache::Idx)
 *   → short-circuit emits empty Page objects from the idx page count.
 *
 * ReaderState::renderPageContents reads the markers directly for layout +
 * drawing.  No ParsedText / TextBlock build, no per-page word-spill.
 *
 * In a build without SNAPIX_MARKERIZER (build-test only) parsePages() produces
 * no pages, mirroring EpubChapterParser's stub behaviour.
 */
class PlainTextParser : public ContentParser {
  std::string filepath_;       // effective content path (SD or LittleFS UTF-8 cache)
  std::string bookCachePath_;  // UnifiedCache dir (Txt::getCachePath())
  GfxRenderer& renderer_;
  RenderConfig config_;
  bool useLittleFs_ = false;
  bool hasMore_ = true;

  // Streaming state (mirrors EpubChapterParser).
  bool markerizeAttempted_ = false;
  bool shortCircuitActive_ = false;
  uint16_t shortCircuitTotalPages_ = 0;
  uint16_t shortCircuitNextPage_ = 0;

  // Real viewport margins plumbed by the caller so the MEASURE-walk config
  // matches ReaderState::renderPageContents (else .idx configHash mismatches).
  int streamingViewportMarginTop_ = 4;
  int streamingViewportMarginBottom_ = 4;
  int streamingViewportMarginLeft_ = 4;
  int streamingViewportMarginRight_ = 4;

  // Markerize the source file into UnifiedCache::Markers (key 0).  One-shot
  // per instance; cache-hit short-circuits.  Returns true on success/hit.
  bool tryMarkerize();

 public:
  // `bookCachePath` is the book's cache directory (Txt::getCachePath()), where
  // the per-book streaming.cache (markers + idx) lives.  `useLittleFs` selects
  // SD vs LittleFS for the source read (cp1251 UTF-8 cache lives on LittleFS).
  PlainTextParser(std::string filepath, std::string bookCachePath, GfxRenderer& renderer,
                  const RenderConfig& config, bool useLittleFs = false);
  ~PlainTextParser() override = default;

  bool parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages = 0,
                  const AbortCallback& shouldAbort = nullptr) override;
  bool hasMoreContent() const override { return hasMore_; }
  bool canResume() const override {
    return shortCircuitActive_ && shortCircuitNextPage_ < shortCircuitTotalPages_;
  }
  void reset() override;

  // Plumb real reader viewport margins (called right after construction), so the
  // upfront MEASURE walk uses the same paginator config as the render path.
  void setStreamingViewport(int marginTop, int marginBottom, int marginLeft, int marginRight) {
    streamingViewportMarginTop_ = marginTop;
    streamingViewportMarginBottom_ = marginBottom;
    streamingViewportMarginLeft_ = marginLeft;
    streamingViewportMarginRight_ = marginRight;
  }
};
