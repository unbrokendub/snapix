/**
 * MarkdownParser.cpp
 *
 * v3.7.0 — streaming parser: markerize the Markdown source via MarkdownStripper
 * into the per-book UnifiedCache, build the page-boundary idx, then short-circuit
 * emit empty Page objects.  Rendering happens off the markers in ReaderState.
 */

#include "MarkdownParser.h"

#include <FS.h>        // Arduino File for LittleFS reads
#include <GfxRenderer.h>
#include <LittleFS.h>  // cp1251 UTF-8 cache lives on LittleFS
#include <Logging.h>
#include <Page.h>
#include <SDCardManager.h>

#include <utility>

#include "StreamingSection.h"  // lib/PageCache — shared idx build + page count

#define TAG "MD_PARSE"

#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
#include <MarkerizeChapter.h>  // MarkerizeReadFn / WriteFn / Status
#include <MarkerizeStream.h>   // CallbackSink + runMarkerizeLoop
#include <UnifiedCache.h>

#include "MarkdownStripper.h"
#endif

namespace {

// SD (FsFile) vs LittleFS (Arduino File) RAII read wrapper — same minimal shape
// as PlainTextParser's.  The MarkdownStripper does its own line buffering, so
// only raw read() is needed here.
class AnyFile {
 public:
  AnyFile() = default;
  AnyFile(const AnyFile&) = delete;
  AnyFile& operator=(const AnyFile&) = delete;
  ~AnyFile() { close(); }

  bool openSd(const std::string& path) {
    if (!SdMan.openFileForRead("MD ", path, sdFile_)) return false;
    useLittleFs_ = false;
    open_ = true;
    return true;
  }
  bool openLittleFs(const std::string& path) {
    lfsFile_ = LittleFS.open(path.c_str(), "r");
    if (!lfsFile_) return false;
    useLittleFs_ = true;
    open_ = true;
    return true;
  }

  int read(uint8_t* buf, size_t len) {
    return useLittleFs_ ? lfsFile_.read(buf, len) : sdFile_.read(buf, len);
  }
  void close() {
    if (!open_) return;
    if (useLittleFs_) {
      lfsFile_.close();
    } else {
      sdFile_.close();
    }
    open_ = false;
  }

 private:
  FsFile sdFile_;
  File lfsFile_;
  bool useLittleFs_ = false;
  bool open_ = false;
};

}  // namespace

MarkdownParser::MarkdownParser(std::string filepath, std::string bookCachePath, GfxRenderer& renderer,
                               const RenderConfig& config, bool useLittleFs)
    : filepath_(std::move(filepath)),
      bookCachePath_(std::move(bookCachePath)),
      renderer_(renderer),
      config_(config),
      useLittleFs_(useLittleFs) {}

MarkdownParser::~MarkdownParser() = default;

void MarkdownParser::reset() {
  hasMore_ = true;
  markerizeAttempted_ = false;
  shortCircuitActive_ = false;
  shortCircuitTotalPages_ = 0;
  shortCircuitNextPage_ = 0;
}

bool MarkdownParser::tryMarkerize() {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  if (markerizeAttempted_) return true;
  markerizeAttempted_ = true;

  auto cache = snapix::unifiedcache::UnifiedCache::shared(bookCachePath_);
  size_t existing = 0;
  if (cache.segmentSize(snapix::unifiedcache::Kind::Markers, 0, &existing)) {
    LOG_INF(TAG, "markerize cache hit (UnifiedCache::Markers size=%zu)", existing);
    return true;
  }

  AnyFile file;
  const bool opened = useLittleFs_ ? file.openLittleFs(filepath_) : file.openSd(filepath_);
  if (!opened) {
    LOG_ERR(TAG, "Failed to open source (%s): %s", useLittleFs_ ? "LittleFS" : "SD", filepath_.c_str());
    return false;
  }

  constexpr size_t kChunkBufBytes = 4096;
  uint8_t chunkBuf[kChunkBufBytes];
  uint32_t inBytes = 0;
  uint32_t outBytes = 0;
  uint16_t chunkCount = 0;
  const snapix::smolport::MarkerizeAbortFn noAbort{};
  snapix::smolport::MarkerizeStatus status = snapix::smolport::MarkerizeStatus::ReadError;

  const bool ok = cache.writeSegmentStreamingDeferred(
      snapix::unifiedcache::Kind::Markers, 0, [&](File& outFile) -> bool {
        snapix::smolport::MarkerizeReadFn readFn = [&file](uint8_t* buf, size_t bufSize) -> int {
          const int r = file.read(buf, bufSize);
          return r < 0 ? -1 : r;  // 0 == clean EOF
        };
        snapix::smolport::MarkerizeWriteFn writeFn = [&outFile](const uint8_t* d, size_t l) -> bool {
          return outFile && outFile.write(d, l) == l;
        };
        snapix::smolport::CallbackSink sink(writeFn, noAbort, outBytes);
        snapix::markdown::MarkdownStripper stripper(sink);
        status = snapix::smolport::runMarkerizeLoop(stripper, sink, readFn, chunkBuf, sizeof(chunkBuf),
                                                    noAbort, inBytes, chunkCount);
        return status == snapix::smolport::MarkerizeStatus::Success;
      });

  file.close();

  if (!ok || status != snapix::smolport::MarkerizeStatus::Success) {
    LOG_ERR(TAG, "markerize failed status=%u in=%u out=%u (UnifiedCache ok=%u)",
            static_cast<unsigned>(status), static_cast<unsigned>(inBytes),
            static_cast<unsigned>(outBytes), static_cast<unsigned>(ok));
    return false;
  }
  LOG_INF(TAG, "markerize done in=%u out=%u chunks=%u", static_cast<unsigned>(inBytes),
          static_cast<unsigned>(outBytes), static_cast<unsigned>(chunkCount));
  return true;
#else
  return false;
#endif
}

bool MarkdownParser::parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete,
                                uint16_t maxPages, const AbortCallback& shouldAbort) {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  (void)shouldAbort;

  auto emitBatch = [&]() -> uint16_t {
    uint16_t created = 0;
    while (shortCircuitNextPage_ < shortCircuitTotalPages_) {
      if (maxPages > 0 && created >= maxPages) break;
      onPageComplete(std::unique_ptr<Page>(new Page));
      ++created;
      ++shortCircuitNextPage_;
    }
    hasMore_ = (shortCircuitNextPage_ < shortCircuitTotalPages_);
    if (!hasMore_) {
      shortCircuitActive_ = false;
      renderer_.clearWidthCache();
    }
    return created;
  };

  if (shortCircuitActive_) {
    return emitBatch() > 0;
  }

  if (!tryMarkerize()) {
    hasMore_ = false;
    return false;
  }

  const uint16_t totalPages = snapix::pagecache::ensureStreamingSectionIdx(
      bookCachePath_, 0, renderer_, config_, streamingViewportMarginTop_,
      streamingViewportMarginBottom_, streamingViewportMarginLeft_, streamingViewportMarginRight_,
      /*hyphenLang=*/"");
  if (totalPages == 0) {
    LOG_ERR(TAG, "idx build produced 0 pages");
    hasMore_ = false;
    return false;
  }

  shortCircuitActive_ = true;
  shortCircuitTotalPages_ = totalPages;
  shortCircuitNextPage_ = 0;
  LOG_INF(TAG, "[STREAM] short-circuit: %u pages from idx", static_cast<unsigned>(totalPages));
  return emitBatch() > 0;
#else
  (void)onPageComplete;
  (void)maxPages;
  (void)shouldAbort;
  hasMore_ = false;
  return false;
#endif
}
