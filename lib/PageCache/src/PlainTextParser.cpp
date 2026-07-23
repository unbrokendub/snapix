#include "PlainTextParser.h"

#include <FS.h>        // Arduino File for LittleFS reads
#include <GfxRenderer.h>
#include <LittleFS.h>  // cp1251 UTF-8 cache lives on LittleFS
#include <Logging.h>
#include <Page.h>
#include <SDCardManager.h>

#include <algorithm>
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
  bool seekTo(uint32_t offset) {
    return useLittleFs_ ? lfsFile_.seek(offset, SeekSet) : sdFile_.seekSet(offset);
  }
  uint32_t size() {
    return useLittleFs_ ? static_cast<uint32_t>(lfsFile_.size())
                        : static_cast<uint32_t>(sdFile_.fileSize());
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

// v3.9.0 — lazy markerize chunk size (source bytes).  Each chunk covers
// ~kChunkBytes of source, extended to the next paragraph boundary so chunks
// split cleanly: TxtStripper(startPendingBreak) re-emits the break the previous
// chunk left pending, making N chunks concatenate byte-identical to one
// continuous markerize (host-proven, TxtStripperTest R7-R10).  ~32 KB → page 0
// of a big TXT after ~5 s instead of ~120 s.
constexpr uint32_t kChunkBytes = 32768;

// Find the end offset of the chunk that starts at `start`: the first PARAGRAPH
// boundary at or after start+kChunkBytes, or fileSize if none remain (the last
// chunk).  The boundary rule MIRRORS TxtStripper exactly:
//   * non-reflow: any '\n' is a paragraph break → cut just after it.
//   * reflow:     a blank line (>= 2 newlines, only '\r' allowed between — a
//                 space/tab is text and does NOT make a blank line, matching the
//                 stripper) → cut just after the second '\n'.
// Cutting at a real paragraph boundary is what makes startPendingBreak correct.
// `file` is repositioned by this call.  Returns an absolute offset in
// (start, fileSize].
uint32_t findChunkEnd(AnyFile& file, bool reflow, uint32_t start, uint32_t fileSize) {
  if (fileSize <= start + kChunkBytes) return fileSize;  // remainder is the last chunk
  const uint32_t base = start + kChunkBytes;
  if (!file.seekTo(base)) return fileSize;

  uint8_t buf[512];
  uint32_t scanned = 0;
  int state = 0;  // reflow: 0 = find first '\n'; 1 = after '\n', seek the blank line's 2nd '\n'
  for (;;) {
    const int n = file.read(buf, sizeof(buf));
    if (n <= 0) return fileSize;  // EOF before a boundary → this is the last chunk
    for (int i = 0; i < n; ++i) {
      const uint8_t b = buf[i];
      if (!reflow) {
        if (b == '\n') return base + scanned + static_cast<uint32_t>(i) + 1;
        continue;
      }
      if (state == 0) {
        if (b == '\n') state = 1;
      } else {  // state 1
        if (b == '\n') return base + scanned + static_cast<uint32_t>(i) + 1;  // blank line
        if (b != '\r') state = 0;  // space/tab/text → soft wrap, not a blank line
      }
    }
    scanned += static_cast<uint32_t>(n);
  }
}

// Re-derive the source start offset of chunk `targetChunk` by applying the chunk
// boundary rule from byte 0, `targetChunk` times.  Used on cold start (fresh
// parser after reboot) to recover srcOffset_ with no persisted cursor — cheap
// because findChunkEnd seeks past each chunk's body and only scans its boundary
// tail.  Returns fileSize if the source has fewer than `targetChunk` chunks.
uint32_t rescanToChunk(AnyFile& file, bool reflow, uint32_t fileSize, int targetChunk) {
  uint32_t off = 0;
  for (int i = 0; i < targetChunk && off < fileSize; ++i) {
    off = findChunkEnd(file, reflow, off, fileSize);
  }
  return off;
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
  initialized_ = false;
  reflow_ = false;
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

bool PlainTextParser::ensureInit(snapix::unifiedcache::UnifiedCache& cache) {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  if (initialized_) return true;

  AnyFile file;
  const bool opened = useLittleFs_ ? file.openLittleFs(filepath_) : file.openSd(filepath_);
  if (!opened) {
    LOG_ERR(TAG, "Failed to open source (%s): %s", useLittleFs_ ? "LittleFS" : "SD", filepath_.c_str());
    return false;
  }
  fileSize_ = file.size();

  // Sample the head to decide paragraph mode (deterministic → matches the mode
  // every chunk was / will be markerized with).
  uint8_t sample[4096];
  const int sampleN = file.read(sample, sizeof(sample));
  reflow_ = sampleN > 0 && sampleHasBlankLines(sample, sampleN);

  // Probe how many Markers chunks already exist → that's where to resume.
  int existing = 0;
  for (;; ++existing) {
    size_t sz = 0;
    if (!cache.segmentSize(snapix::unifiedcache::Kind::Markers, static_cast<uint16_t>(existing), &sz)) {
      break;
    }
  }
  chunkIdx_ = existing;

  // Re-derive the source offset of the next chunk (cold start).  A fresh book
  // (existing == 0) starts at byte 0.
  srcOffset_ = (existing > 0) ? rescanToChunk(file, reflow_, fileSize_, existing) : 0;
  sourceExhausted_ = (fileSize_ == 0) || (srcOffset_ >= fileSize_);

  // Legacy guard (pre-v3.9.0): a v3.8.0 cache holds the WHOLE file in a single
  // Markers key 0, which probes as "1 chunk" but actually spans the entire
  // source.  Resuming at chunk 1 would append overlapping markers and corrupt
  // the stream.  Detect it — the existing key-0 segment is far larger than a
  // fresh chunk 0 (capped near kChunkBytes) — and treat it as ONE complete
  // chunk: render via the 1-chunk reader + the existing idx, never markerize
  // more.  (A fresh chunk 0 with a giant first paragraph grows srcOffset with
  // it, so its segment stays ~srcOffset and won't trip this.)
  if (existing == 1 && !sourceExhausted_) {
    size_t k0 = 0;
    if (cache.segmentSize(snapix::unifiedcache::Kind::Markers, 0, &k0) &&
        k0 > static_cast<size_t>(srcOffset_) + kChunkBytes) {
      LOG_INF(TAG, "legacy whole-file markers (size=%zu chunk0End=%u) — single complete chunk", k0,
              static_cast<unsigned>(srcOffset_));
      srcOffset_ = fileSize_;
      sourceExhausted_ = true;
    }
  }
  file.close();

  // Re-use the already-built idx (if version + config still match) so a cold
  // parser doesn't re-measure chunks it already indexed.
  pagesAvailable_ = 0;
  if (existing > 0) {
    const uint16_t loaded = snapix::pagecache::loadChunkedSectionIdx(
        idxState_, cache, renderer_, config_, streamingViewportMarginTop_,
        streamingViewportMarginBottom_, streamingViewportMarginLeft_, streamingViewportMarginRight_,
        /*hyphenLang=*/"");
    if (idxState_.configHashValid) {
      pagesAvailable_ = static_cast<uint16_t>(loaded + (sourceExhausted_ ? 1 : 0));
    }
  }

  emitCursor_ = 0;
  initialized_ = true;
  LOG_INF(TAG, "init reflow=%u size=%u chunks=%d srcOff=%u exhausted=%u pagesAvail=%u",
          static_cast<unsigned>(reflow_), static_cast<unsigned>(fileSize_), existing,
          static_cast<unsigned>(srcOffset_), static_cast<unsigned>(sourceExhausted_),
          static_cast<unsigned>(pagesAvailable_));
  return true;
#else
  return false;  // markerizer compiled out
#endif
}

bool PlainTextParser::markerizeNextChunk(snapix::unifiedcache::UnifiedCache& cache) {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  if (sourceExhausted_) return false;

  AnyFile file;
  const bool opened = useLittleFs_ ? file.openLittleFs(filepath_) : file.openSd(filepath_);
  if (!opened) {
    LOG_ERR(TAG, "markerize: failed to open source: %s", filepath_.c_str());
    return false;
  }

  const uint32_t start = srcOffset_;
  const uint32_t end = findChunkEnd(file, reflow_, start, fileSize_);
  if (end <= start) {  // nothing left (defensive)
    file.close();
    sourceExhausted_ = true;
    return false;
  }
  if (!file.seekTo(start)) {
    file.close();
    return false;
  }

  constexpr size_t kChunkBufBytes = 4096;
  uint8_t chunkBuf[kChunkBufBytes];
  uint32_t inBytes = 0;
  uint32_t outBytes = 0;
  uint16_t loopChunks = 0;
  uint32_t remaining = end - start;  // cap the markerize at this chunk's source range
  const int thisChunk = chunkIdx_;
  const snapix::smolport::MarkerizeAbortFn noAbort{};
  snapix::smolport::MarkerizeStatus status = snapix::smolport::MarkerizeStatus::ReadError;

  const bool ok = cache.writeSegmentStreamingDeferred(
      snapix::unifiedcache::Kind::Markers, static_cast<uint16_t>(thisChunk),
      [&](File& outFile) -> bool {
        snapix::smolport::MarkerizeReadFn readFn = [&file, &remaining](uint8_t* buf,
                                                                       size_t bufSize) -> int {
          if (remaining == 0) return 0;  // chunk range consumed → clean EOF
          const size_t want = std::min(bufSize, static_cast<size_t>(remaining));
          const int r = file.read(buf, want);
          if (r > 0) remaining -= static_cast<uint32_t>(r);
          return r < 0 ? -1 : r;
        };
        snapix::smolport::MarkerizeWriteFn writeFn = [&outFile](const uint8_t* d, size_t l) -> bool {
          return outFile && outFile.write(d, l) == l;
        };
        snapix::smolport::CallbackSink sink(writeFn, noAbort, outBytes);
        // Every chunk after the first begins as if a paragraph break is pending,
        // so it re-emits the break the previous chunk left at its boundary.
        snapix::smolport::TxtStripper stripper(sink, reflow_, /*startPendingBreak=*/thisChunk > 0);
        status = snapix::smolport::runMarkerizeLoop(stripper, sink, readFn, chunkBuf, sizeof(chunkBuf),
                                                    noAbort, inBytes, loopChunks);
        return status == snapix::smolport::MarkerizeStatus::Success;
      });

  file.close();

  if (!ok || status != snapix::smolport::MarkerizeStatus::Success) {
    LOG_ERR(TAG, "markerize chunk %d failed status=%u in=%u out=%u", thisChunk,
            static_cast<unsigned>(status), static_cast<unsigned>(inBytes),
            static_cast<unsigned>(outBytes));
    return false;
  }

  srcOffset_ = end;
  sourceExhausted_ = (end >= fileSize_);
  LOG_INF(TAG, "chunk %d markerized src=[%u,%u) in=%u out=%u exhausted=%u", thisChunk,
          static_cast<unsigned>(start), static_cast<unsigned>(end), static_cast<unsigned>(inBytes),
          static_cast<unsigned>(outBytes), static_cast<unsigned>(sourceExhausted_));
  ++chunkIdx_;
  return true;
#else
  return false;  // markerizer compiled out
#endif
}

bool PlainTextParser::parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete,
                                 uint16_t maxPages, const AbortCallback& shouldAbort) {
#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
  (void)shouldAbort;  // markerize is per-chunk; abort is honoured between calls

  auto cache = snapix::unifiedcache::UnifiedCache::shared(bookCachePath_);
  if (!ensureInit(cache)) {
    hasMore_ = false;
    return false;
  }

  // Markers can survive an interrupted/failed idx write.  Re-measure the
  // already-published chunks instead of getting stuck after reboot with source
  // exhausted but zero pages available.
  if (chunkIdx_ > 0 && !idxState_.configHashValid) {
    pagesAvailable_ = snapix::pagecache::extendChunkedSectionIdx(
        idxState_, cache, sourceExhausted_, renderer_, config_,
        streamingViewportMarginTop_, streamingViewportMarginBottom_,
        streamingViewportMarginLeft_, streamingViewportMarginRight_,
        /*hyphenLang=*/"");
  }

  // Make pages available to emit: if the emit cursor has caught up to what the
  // idx covers and there's still source left, markerize the NEXT chunk and
  // extend the idx over it.  One chunk per iteration; loop (bounded) in case a
  // chunk yields no new complete page (e.g. a single huge paragraph).
  uint16_t guard = 0;
  while (emitCursor_ >= pagesAvailable_ && !sourceExhausted_) {
    if (!markerizeNextChunk(cache)) break;  // markerize failure → stop growing (emit what we have)
    const uint16_t avail = snapix::pagecache::extendChunkedSectionIdx(
        idxState_, cache, sourceExhausted_, renderer_, config_, streamingViewportMarginTop_,
        streamingViewportMarginBottom_, streamingViewportMarginLeft_, streamingViewportMarginRight_,
        /*hyphenLang=*/"");
    if (avail == 0) {
      if (!sourceExhausted_ && ++guard <= 8192) continue;
      LOG_ERR(TAG, "[STREAM] idx extend produced 0 pages (chunk %d)", chunkIdx_ - 1);
      break;
    }
    pagesAvailable_ = avail;
    if (++guard > 8192) break;  // belt-and-suspenders against a non-growing loop
  }

  // Emit the available pages (empty Page objects — ReaderState renders from the
  // markers via the idx), up to maxPages for this batch.
  uint16_t created = 0;
  while (emitCursor_ < pagesAvailable_) {
    if (maxPages > 0 && created >= maxPages) break;
    onPageComplete(std::unique_ptr<Page>(new Page));
    ++emitCursor_;
    ++created;
  }

  hasMore_ = (emitCursor_ < pagesAvailable_) || !sourceExhausted_;
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
