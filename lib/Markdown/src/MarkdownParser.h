/**
 * MarkdownParser.h
 *
 * v3.7.0 — migrated from the legacy ParsedText block path to the v3 streaming
 * pipeline (same as EPUB/FB2 and PlainTextParser):
 *   tryMarkerize()  (file → MarkdownStripper → UnifiedCache::Markers key 0)
 *   → ensureStreamingSectionIdx()  (MEASURE-only walk → UnifiedCache::Idx)
 *   → short-circuit emits empty Page objects from the idx page count.
 *
 * ReaderState::renderPageContents reads the markers directly for layout +
 * drawing.  The md_parser tokeniser still does Markdown syntax recognition —
 * now inside MarkdownStripper, emitting markers instead of ParsedText blocks.
 */

#pragma once

#include <ContentParser.h>
#include <RenderConfig.h>
#include <SdFat.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class Page;
class GfxRenderer;

class MarkdownParser : public ContentParser {
 public:
  // `bookCachePath` is the book's cache directory (Markdown::getCachePath()).
  // `useLittleFs` selects SD vs LittleFS for the source read (cp1251 UTF-8
  // cache lives on LittleFS).
  MarkdownParser(std::string filepath, std::string bookCachePath, GfxRenderer& renderer,
                 const RenderConfig& config, bool useLittleFs = false);
  ~MarkdownParser() override;

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

 private:
  std::string filepath_;       // effective content path (SD or LittleFS UTF-8 cache)
  std::string bookCachePath_;  // UnifiedCache dir (Markdown::getCachePath())
  GfxRenderer& renderer_;
  RenderConfig config_;
  bool useLittleFs_ = false;
  bool hasMore_ = true;

  // Streaming state (mirrors EpubChapterParser / PlainTextParser).
  bool markerizeAttempted_ = false;
  bool shortCircuitActive_ = false;
  uint16_t shortCircuitTotalPages_ = 0;
  uint16_t shortCircuitNextPage_ = 0;

  int streamingViewportMarginTop_ = 4;
  int streamingViewportMarginBottom_ = 4;
  int streamingViewportMarginLeft_ = 4;
  int streamingViewportMarginRight_ = 4;

  bool tryMarkerize();
};
