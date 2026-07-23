#pragma once

#include <ContentParser.h>
#include <RenderConfig.h>
#include <StreamingSection.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class GfxRenderer;
class Page;
namespace snapix::unifiedcache {
class UnifiedCache;
}

// Streaming parser for a standalone HTML file.  The source is markerized
// directly from SD into the book's UnifiedCache, then the shared streaming
// paginator builds the page-boundary index used by ReaderState.
class HtmlParser : public ContentParser {
 public:
  HtmlParser(std::string filepath, std::string bookCachePath, GfxRenderer& renderer,
             const RenderConfig& config);
  ~HtmlParser() override;

  bool parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete,
                  uint16_t maxPages = 0,
                  const AbortCallback& shouldAbort = nullptr) override;
  bool hasMoreContent() const override { return hasMore_; }
  bool canResume() const override {
    return initialized_ && (emitCursor_ < pagesAvailable_ || !sourceExhausted_);
  }
  void reset() override;

  void setStreamingViewport(int marginTop, int marginBottom, int marginLeft, int marginRight) {
    streamingViewportMarginTop_ = marginTop;
    streamingViewportMarginBottom_ = marginBottom;
    streamingViewportMarginLeft_ = marginLeft;
    streamingViewportMarginRight_ = marginRight;
  }

 private:
  struct ProgressiveMarkerizer;
  bool ensureInit(snapix::unifiedcache::UnifiedCache& cache,
                  const AbortCallback& shouldAbort);
  bool restoreMarkerizer(const AbortCallback& shouldAbort);
  bool markerizeNextChunk(snapix::unifiedcache::UnifiedCache& cache,
                          const AbortCallback& shouldAbort);

  std::string filepath_;
  std::string bookCachePath_;
  GfxRenderer& renderer_;
  RenderConfig config_;
  bool hasMore_ = true;
  std::unique_ptr<ProgressiveMarkerizer> markerizer_;
  bool initialized_ = false;
  uint32_t fileSize_ = 0;
  int chunkIdx_ = 0;
  uint32_t srcOffset_ = 0;
  bool sourceExhausted_ = false;
  snapix::pagecache::ChunkedIdxState idxState_;
  uint16_t pagesAvailable_ = 0;
  uint16_t emitCursor_ = 0;

  int streamingViewportMarginTop_ = 4;
  int streamingViewportMarginBottom_ = 4;
  int streamingViewportMarginLeft_ = 4;
  int streamingViewportMarginRight_ = 4;
};
