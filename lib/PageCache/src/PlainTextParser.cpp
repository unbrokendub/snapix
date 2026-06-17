#include "PlainTextParser.h"

#include <FS.h>        // Arduino File for LittleFS reads
#include <GfxRenderer.h>
#include <LittleFS.h>  // cp1251 UTF-8 cache lives on LittleFS
#include <Logging.h>
#include <Page.h>
#include <SDCardManager.h>

#include <utility>

#include "StreamingSection.h"

#define TAG "TXT_PARSE"

#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
#include <MarkerizeChapter.h>  // MarkerizeReadFn / WriteFn / Status
#include <MarkerizeStream.h>   // CallbackSink + runMarkerizeLoop
#include <TxtStripper.h>
#include <UnifiedCache.h>
#endif

namespace {

// v2.0.189 — tiny RAII wrapper abstracting SD (FsFile) vs LittleFS (Arduino
// File) so the markerize read loop works regardless of which FS holds the
// content.  (Retained from the legacy parser; the cp1251 UTF-8 cache lives on
// LittleFS while plain UTF-8 sources stay on SD.)
class AnyFile {
 public:
  AnyFile() = default;
  AnyFile(const AnyFile&) = delete;
  AnyFile& operator=(const AnyFile&) = delete;
  ~AnyFile() { close(); }

  bool openSd(const std::string& path) {
    if (!SdMan.openFileForRead("TXT", path, sdFile_)) return false;
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
  bool rewind() {
    // SdFat: seekSet(absolute); Arduino File: seek(pos, SeekSet).
    return useLittleFs_ ? lfsFile_.seek(0, SeekSet) : sdFile_.seekSet(0);
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

// True if the sample contains a BLANK line (a newline followed by only
// whitespace up to the next newline).  When present, blank lines are the
// paragraph separators, so single newlines are soft wraps → TxtStripper reflow
// mode.  When absent, every line is its own paragraph (no reflow).
bool sampleHasBlankLines(const uint8_t* buf, int len) {
  for (int i = 0; i < len; ++i) {
    if (buf[i] != '\n') continue;
    int j = i + 1;
    while (j < len && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\r')) ++j;
    if (j < len && buf[j] == '\n') return true;
  }
  return false;
}

}  // namespace

PlainTextParser::PlainTextParser(std::string filepath, std::string bookCachePath, GfxRenderer& renderer,
                                 const RenderConfig& config, bool useLittleFs)
    : filepath_(std::move(filepath)),
      bookCachePath_(std::move(bookCachePath)),
      renderer_(renderer),
      config_(config),
      useLittleFs_(useLittleFs) {}

void PlainTextParser::reset() {
  hasMore_ = true;
  markerizeAttempted_ = false;
  shortCircuitActive_ = false;
  shortCircuitTotalPages_ = 0;
  shortCircuitNextPage_ = 0;
}

bool PlainTextParser::tryMarkerize() {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  if (markerizeAttempted_) return true;
  markerizeAttempted_ = true;

  auto cache = snapix::unifiedcache::UnifiedCache::shared(bookCachePath_);
  size_t existing = 0;
  if (cache.segmentSize(snapix::unifiedcache::Kind::Markers, 0, &existing)) {
    LOG_INF(TAG, "markerize cache hit (UnifiedCache::Markers size=%zu)", existing);
    return true;  // markers already present
  }

  AnyFile file;
  const bool opened = useLittleFs_ ? file.openLittleFs(filepath_) : file.openSd(filepath_);
  if (!opened) {
    LOG_ERR(TAG, "Failed to open source (%s): %s", useLittleFs_ ? "LittleFS" : "SD", filepath_.c_str());
    return false;
  }

  constexpr size_t kChunkBufBytes = 4096;
  uint8_t chunkBuf[kChunkBufBytes];

  // Sample the head to decide paragraph mode: hard-wrapped text (blank-line
  // separators present) reflows single newlines into paragraphs; otherwise each
  // line is its own paragraph.  Reuse chunkBuf for the sample, then rewind.
  const int sampleN = file.read(chunkBuf, sizeof(chunkBuf));
  const bool reflow = sampleN > 0 && sampleHasBlankLines(chunkBuf, sampleN);
  file.rewind();
  LOG_INF(TAG, "markerize start reflow=%u (sample=%d)", static_cast<unsigned>(reflow), sampleN);

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
        snapix::smolport::TxtStripper stripper(sink, reflow);
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
  return false;  // markerizer compiled out
#endif
}

bool PlainTextParser::parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete,
                                 uint16_t maxPages, const AbortCallback& shouldAbort) {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  (void)shouldAbort;  // markerize/short-circuit not interrupted mid-pass

  // Continuation: keep emitting empty Pages from a previously-built idx.
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

  // Init: markerize, build idx, activate short-circuit.
  if (!tryMarkerize()) {
    hasMore_ = false;
    return false;
  }

  // TXT has no declared language → no hyphenation (avoids wrong-language
  // breaks and keeps MEASURE/DRAW config identical).
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
