#include "MarkerizeChapter.h"

#include <cstring>

namespace snapix::smolport {

namespace {

// Stripper sink that forwards emitted marker bytes to a caller's writeChunk
// callback.  Tracks last-write status so the markerize loop can latch a
// write error and bail early.  Implements shouldStop() against the
// caller's abort hook so the stripper itself bails between bytes when
// user input fires.
//
// v3.1.1 — WRITE COALESCING.  HtmlStripper emits plain text BYTE-BY-BYTE
// (`sink_.emit(&b, 1)` per character) and markers in 2-4 byte dribbles.
// Pre-fix, every emit went straight through writeChunk → one LittleFS
// `File::write()` call per byte of the book — a 430 KB FB2 section issued
// ~420 000 single-byte writes, and the per-call overhead (Arduino File →
// VFS → littlefs cache lookup) dominated cold markerize wall time (hardware
// repro: 116 s for that section, ~3.7 KB/s).  This sink now stages emits
// into a 1 KB buffer and flushes in bulk, collapsing the write-call count
// by ~3 orders of magnitude.  Output BYTES are identical — only the call
// granularity changes — so markers segments, idx files, and page caches
// stay valid (no version bump needed).
//
// flush() MUST be called after stripper.finish() on the clean-EOF path —
// markerizeChapter does.  Error/abort exits skip the flush: the enclosing
// UnifiedCache streaming frame is abandoned (size never patched) so
// unflushed tail bytes are irrelevant.
class CallbackSink : public HtmlStripperSink {
 public:
  CallbackSink(MarkerizeWriteFn& writeChunk, const MarkerizeAbortFn& shouldAbort,
               uint32_t& outBytesAccum)
      : writeChunk_(writeChunk), shouldAbort_(shouldAbort), outBytesAccum_(outBytesAccum) {}

  void emit(const uint8_t* data, size_t len) override {
    if (writeFailed_) return;  // latch — drop subsequent emits
    // Large emits (image paths / anchor payloads near the 512 B payload
    // cap) bypass staging: flush what's pending, then write directly.
    if (len >= kStageCapacity) {
      flush();
      if (writeFailed_) return;
      if (!writeChunk_(data, len)) {
        writeFailed_ = true;
        return;
      }
      outBytesAccum_ += static_cast<uint32_t>(len);
      return;
    }
    if (stageLen_ + len > kStageCapacity) {
      flush();
      if (writeFailed_) return;
    }
    std::memcpy(stage_ + stageLen_, data, len);
    stageLen_ = static_cast<uint16_t>(stageLen_ + len);
    outBytesAccum_ += static_cast<uint32_t>(len);
  }

  // Push any staged bytes through writeChunk.  Idempotent when empty.
  void flush() {
    if (writeFailed_ || stageLen_ == 0) return;
    if (!writeChunk_(stage_, stageLen_)) {
      writeFailed_ = true;
    }
    stageLen_ = 0;
  }

  // Called by the stripper between bytes.  Forward to the caller's abort.
  bool shouldStop() const override {
    if (writeFailed_) return true;
    return shouldAbort_ && shouldAbort_();
  }

  bool writeFailed() const { return writeFailed_; }

 private:
  static constexpr size_t kStageCapacity = 1024;
  MarkerizeWriteFn& writeChunk_;
  const MarkerizeAbortFn& shouldAbort_;
  uint32_t& outBytesAccum_;
  uint8_t stage_[kStageCapacity];
  uint16_t stageLen_ = 0;
  bool writeFailed_ = false;
};

}  // namespace

MarkerizeStatus markerizeChapter(HtmlStripper::Mode mode, MarkerizeReadFn readChunk,
                                  MarkerizeWriteFn writeChunk, uint8_t* chunkBuf, size_t chunkBufSize,
                                  const MarkerizeAbortFn& shouldAbort, MarkerizeStats* outStats) {
  if (!readChunk || !writeChunk || chunkBuf == nullptr || chunkBufSize == 0) {
    if (outStats) {
      outStats->inputBytes = 0;
      outStats->outputBytes = 0;
      outStats->chunksProcessed = 0;
    }
    return MarkerizeStatus::ReadError;  // misconfigured caller — treat as setup error
  }

  uint32_t inBytes = 0;
  uint32_t outBytes = 0;
  uint16_t chunkCount = 0;

  CallbackSink sink(writeChunk, shouldAbort, outBytes);
  HtmlStripper stripper(sink, mode);

  MarkerizeStatus result = MarkerizeStatus::Success;

  for (;;) {
    // Cooperative abort check between chunks (the stripper-internal
    // shouldStop() handles between-byte aborts; this is the coarser
    // outer check that fires even if the previous chunk produced no
    // markers).
    if (shouldAbort && shouldAbort()) {
      result = MarkerizeStatus::Aborted;
      break;
    }

    const int got = readChunk(chunkBuf, chunkBufSize);
    if (got == 0) {
      // Clean EOF.
      stripper.finish();
      // v3.1.1 — drain the coalescing stage so the markers segment holds
      // every emitted byte.  A flush failure is a write error like any
      // other (the enclosing streaming frame gets abandoned).
      sink.flush();
      if (sink.writeFailed()) {
        result = MarkerizeStatus::WriteError;
      }
      break;
    }
    if (got < 0) {
      result = MarkerizeStatus::ReadError;
      break;
    }

    const size_t consumed = stripper.feed(chunkBuf, static_cast<size_t>(got));
    inBytes += static_cast<uint32_t>(consumed);
    ++chunkCount;

    if (sink.writeFailed()) {
      result = MarkerizeStatus::WriteError;
      break;
    }

    // Stripper returned consumed < got iff the sink asked it to stop.
    // That stop happens via shouldAbort or write-failure; both are
    // captured above, so consumed < got here means an abort latched
    // mid-chunk and we should stop the outer loop.
    if (consumed < static_cast<size_t>(got)) {
      // If shouldAbort triggered the stop, classify as Aborted; else as
      // WriteError (sink propagated write-failure via shouldStop).
      if (sink.writeFailed()) {
        result = MarkerizeStatus::WriteError;
      } else {
        result = MarkerizeStatus::Aborted;
      }
      break;
    }
  }

  if (outStats) {
    outStats->inputBytes = inBytes;
    outStats->outputBytes = outBytes;
    outStats->chunksProcessed = chunkCount;
  }
  return result;
}

}  // namespace snapix::smolport
