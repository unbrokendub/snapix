#include "HtmlParser.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Page.h>
#include <SDCardManager.h>
#include <SharedSpiLock.h>

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

#include "ProgressiveMarkerizer.h"

#define TAG "HTML_PARSE"

#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
#include <HtmlStripper.h>
#include <UnifiedCache.h>
#endif

namespace {

constexpr uint32_t kSourceChunkBytes = 32768;
constexpr size_t kReadBufferBytes = 4096;

class SdSource {
 public:
  ~SdSource() { close(); }
  bool open(const std::string& path) {
    open_ = SdMan.openFileForRead("HTML", path, file_);
    return open_;
  }
  int read(uint8_t* buf, size_t len) {
    snapix::spi::SharedBusLock lock;
    return file_.read(buf, len);
  }
  bool seekTo(uint32_t offset) {
    snapix::spi::SharedBusLock lock;
    return file_.seek(offset);
  }
  uint32_t size() {
    snapix::spi::SharedBusLock lock;
    return static_cast<uint32_t>(file_.fileSize());
  }
  void close() {
    if (!open_) return;
    file_.close();
    open_ = false;
  }

 private:
  FsFile file_;
  bool open_ = false;
};

}  // namespace

#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
struct HtmlParser::ProgressiveMarkerizer {
  snapix::pagecache::StatefulMarkerizer<snapix::smolport::HtmlStripper> impl;
};
#else
struct HtmlParser::ProgressiveMarkerizer {};
#endif

HtmlParser::HtmlParser(std::string filepath, std::string bookCachePath,
                       GfxRenderer& renderer, const RenderConfig& config)
    : filepath_(std::move(filepath)),
      bookCachePath_(std::move(bookCachePath)),
      renderer_(renderer),
      config_(config) {}

HtmlParser::~HtmlParser() = default;

void HtmlParser::reset() {
  hasMore_ = true;
  markerizer_.reset();
  initialized_ = false;
  fileSize_ = 0;
  chunkIdx_ = 0;
  srcOffset_ = 0;
  sourceExhausted_ = false;
  idxState_.boundaries.clear();
  idxState_.configHash = 0;
  idxState_.configHashValid = false;
  pagesAvailable_ = 0;
  emitCursor_ = 0;
}

bool HtmlParser::restoreMarkerizer(const AbortCallback& shouldAbort) {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  std::unique_ptr<ProgressiveMarkerizer> rebuilt(
      new (std::nothrow) ProgressiveMarkerizer);
  if (!rebuilt) return false;

  SdSource source;
  if (!source.open(filepath_)) return false;
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kReadBufferBytes]);
  if (!buf) return false;

  uint32_t cursor = 0;
  for (int chunk = 0; chunk < chunkIdx_; ++chunk) {
    rebuilt->impl.begin(nullptr, shouldAbort);
    do {
      const uint32_t end =
          std::min<uint32_t>(fileSize_, cursor + kSourceChunkBytes);
      while (cursor < end) {
        if (shouldAbort && shouldAbort()) return false;
        const size_t want =
            std::min<size_t>(kReadBufferBytes, static_cast<size_t>(end - cursor));
        const int n = source.read(buf.get(), want);
        if (n <= 0) return false;
        const size_t consumed =
            rebuilt->impl.feed(buf.get(), static_cast<size_t>(n));
        if (consumed != static_cast<size_t>(n)) return false;
        cursor += static_cast<uint32_t>(n);
      }
    } while (rebuilt->impl.produced() == 0 && cursor < fileSize_);

    if (cursor >= fileSize_) rebuilt->impl.finish();
    if (!rebuilt->impl.ok() || rebuilt->impl.produced() == 0) return false;
    rebuilt->impl.detach();
  }

  srcOffset_ = cursor;
  sourceExhausted_ = fileSize_ == 0 || srcOffset_ >= fileSize_;
  markerizer_ = std::move(rebuilt);
  return true;
#else
  (void)shouldAbort;
  return false;
#endif
}

bool HtmlParser::ensureInit(snapix::unifiedcache::UnifiedCache& cache,
                            const AbortCallback& shouldAbort) {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  if (initialized_) return markerizer_ != nullptr || sourceExhausted_;

  SdSource source;
  if (!source.open(filepath_)) {
    LOG_ERR(TAG, "Failed to open source: %s", filepath_.c_str());
    return false;
  }
  fileSize_ = source.size();
  source.close();

  int existing = 0;
  for (;; ++existing) {
    size_t size = 0;
    if (!cache.segmentSize(snapix::unifiedcache::Kind::Markers,
                           static_cast<uint16_t>(existing), &size)) {
      break;
    }
  }
  chunkIdx_ = existing;
  srcOffset_ = 0;
  sourceExhausted_ = fileSize_ == 0;
  if (!restoreMarkerizer(shouldAbort)) {
    if (fileSize_ != 0 || existing != 0) return false;
  }

  pagesAvailable_ = 0;
  if (existing > 0) {
    const uint16_t loaded = snapix::pagecache::loadChunkedSectionIdx(
        idxState_, cache, renderer_, config_, streamingViewportMarginTop_,
        streamingViewportMarginBottom_, streamingViewportMarginLeft_,
        streamingViewportMarginRight_, /*hyphenLang=*/"");
    if (idxState_.configHashValid) {
      pagesAvailable_ =
          static_cast<uint16_t>(loaded + (sourceExhausted_ ? 1 : 0));
    }
  }

  emitCursor_ = 0;
  initialized_ = true;
  LOG_INF(TAG, "progressive init size=%u chunks=%d srcOff=%u exhausted=%u pages=%u",
          static_cast<unsigned>(fileSize_), existing,
          static_cast<unsigned>(srcOffset_),
          static_cast<unsigned>(sourceExhausted_),
          static_cast<unsigned>(pagesAvailable_));
  return true;
#else
  (void)cache;
  (void)shouldAbort;
  return false;
#endif
}

bool HtmlParser::markerizeNextChunk(
    snapix::unifiedcache::UnifiedCache& cache,
    const AbortCallback& shouldAbort) {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  if (sourceExhausted_) return false;
  if (!markerizer_ && !restoreMarkerizer(shouldAbort)) return false;

  SdSource source;
  if (!source.open(filepath_) || !source.seekTo(srcOffset_)) return false;
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kReadBufferBytes]);
  if (!buf) return false;

  const uint32_t start = srcOffset_;
  uint32_t newOffset = start;
  bool reachedEof = false;
  bool aborted = false;
  const int thisChunk = chunkIdx_;
  const bool ok = cache.writeSegmentStreamingDeferred(
      snapix::unifiedcache::Kind::Markers, static_cast<uint16_t>(thisChunk),
      [&](File& outFile) -> bool {
        markerizer_->impl.begin(&outFile, shouldAbort);
        do {
          const uint32_t end =
              std::min<uint32_t>(fileSize_, newOffset + kSourceChunkBytes);
          while (newOffset < end) {
            if (shouldAbort && shouldAbort()) {
              aborted = true;
              markerizer_->impl.detach();
              return false;
            }
            const size_t want = std::min<size_t>(
                kReadBufferBytes, static_cast<size_t>(end - newOffset));
            const int n = source.read(buf.get(), want);
            if (n <= 0) {
              markerizer_->impl.detach();
              return false;
            }
            const size_t consumed =
                markerizer_->impl.feed(buf.get(), static_cast<size_t>(n));
            if (consumed != static_cast<size_t>(n)) {
              aborted = shouldAbort && shouldAbort();
              markerizer_->impl.detach();
              return false;
            }
            newOffset += static_cast<uint32_t>(n);
          }
        } while (markerizer_->impl.produced() == 0 && newOffset < fileSize_);

        reachedEof = newOffset >= fileSize_;
        if (reachedEof) markerizer_->impl.finish();
        const bool success =
            markerizer_->impl.ok() && markerizer_->impl.produced() > 0;
        markerizer_->impl.detach();
        return success;
      });
  source.close();

  if (!ok) {
    markerizer_.reset();
    if (reachedEof && newOffset >= fileSize_) {
      srcOffset_ = fileSize_;
      sourceExhausted_ = true;
    }
    LOG_INF(TAG, "markerize chunk %d stopped src=[%u,%u) abort=%u",
            thisChunk, static_cast<unsigned>(start),
            static_cast<unsigned>(newOffset), static_cast<unsigned>(aborted));
    return false;
  }

  srcOffset_ = newOffset;
  sourceExhausted_ = reachedEof;
  ++chunkIdx_;
  LOG_INF(TAG, "chunk %d markerized src=[%u,%u) exhausted=%u", thisChunk,
          static_cast<unsigned>(start), static_cast<unsigned>(srcOffset_),
          static_cast<unsigned>(sourceExhausted_));
  return true;
#else
  (void)cache;
  (void)shouldAbort;
  return false;
#endif
}

bool HtmlParser::parsePages(
    const std::function<void(std::unique_ptr<Page>)>& onPageComplete,
    uint16_t maxPages, const AbortCallback& shouldAbort) {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  auto cache = snapix::unifiedcache::UnifiedCache::shared(bookCachePath_);
  if (!ensureInit(cache, shouldAbort)) {
    hasMore_ = shouldAbort && shouldAbort();
    return false;
  }

  if (chunkIdx_ > 0 && !idxState_.configHashValid) {
    pagesAvailable_ = snapix::pagecache::extendChunkedSectionIdx(
        idxState_, cache, sourceExhausted_, renderer_, config_,
        streamingViewportMarginTop_, streamingViewportMarginBottom_,
        streamingViewportMarginLeft_, streamingViewportMarginRight_,
        /*hyphenLang=*/"");
  }

  uint16_t guard = 0;
  while (emitCursor_ >= pagesAvailable_ && !sourceExhausted_) {
    if (shouldAbort && shouldAbort()) break;
    const bool wroteChunk = markerizeNextChunk(cache, shouldAbort);
    if (!wroteChunk && !sourceExhausted_) break;
    const uint16_t available = snapix::pagecache::extendChunkedSectionIdx(
        idxState_, cache, sourceExhausted_, renderer_, config_,
        streamingViewportMarginTop_, streamingViewportMarginBottom_,
        streamingViewportMarginLeft_, streamingViewportMarginRight_,
        /*hyphenLang=*/"");
    if (available == 0) {
      if (!sourceExhausted_ && ++guard <= 8192) continue;
      break;
    }
    pagesAvailable_ = available;
    if (++guard > 8192) break;
  }

  uint16_t created = 0;
  while (emitCursor_ < pagesAvailable_) {
    if (shouldAbort && shouldAbort()) break;
    if (maxPages > 0 && created >= maxPages) break;
    onPageComplete(std::unique_ptr<Page>(new Page));
    ++emitCursor_;
    ++created;
  }

  hasMore_ = emitCursor_ < pagesAvailable_ || !sourceExhausted_;
  if (!hasMore_) renderer_.clearWidthCache();
  return created > 0;
#else
  (void)onPageComplete;
  (void)maxPages;
  (void)shouldAbort;
  hasMore_ = false;
  return false;
#endif
}
