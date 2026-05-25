#include "MarkerizeChapter.h"

namespace snapix::smolport {

namespace {

// Stripper sink that forwards emitted marker bytes to a caller's writeChunk
// callback.  Tracks last-write status so the markerize loop can latch a
// write error and bail early.  Implements shouldStop() against the
// caller's abort hook so the stripper itself bails between bytes when
// user input fires.
class CallbackSink : public HtmlStripperSink {
 public:
  CallbackSink(MarkerizeWriteFn& writeChunk, const MarkerizeAbortFn& shouldAbort,
               uint32_t& outBytesAccum)
      : writeChunk_(writeChunk), shouldAbort_(shouldAbort), outBytesAccum_(outBytesAccum) {}

  void emit(const uint8_t* data, size_t len) override {
    if (writeFailed_) return;  // latch — drop subsequent emits
    if (!writeChunk_(data, len)) {
      writeFailed_ = true;
      return;
    }
    outBytesAccum_ += static_cast<uint32_t>(len);
  }

  // Called by the stripper between bytes.  Forward to the caller's abort.
  bool shouldStop() const override {
    if (writeFailed_) return true;
    return shouldAbort_ && shouldAbort_();
  }

  bool writeFailed() const { return writeFailed_; }

 private:
  MarkerizeWriteFn& writeChunk_;
  const MarkerizeAbortFn& shouldAbort_;
  uint32_t& outBytesAccum_;
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
