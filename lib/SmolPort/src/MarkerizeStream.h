#pragma once

// =============================================================================
// MarkerizeStream — the format-agnostic core of the markerize pass.
//
// `markerizeChapter()` (MarkerizeChapter.cpp) drives an HtmlStripper over an
// HTML/FB2 byte stream.  The read→feed→write loop and the write-coalescing
// sink underneath it are NOT html-specific, though — any "stripper" with a
//
//     size_t feed(const uint8_t* data, size_t len);   // returns bytes consumed
//     void   finish();
//
// interface that emits marker bytes to an HtmlStripperSink can reuse them.
// TXT (TxtStripper) and Markdown (MarkdownStripper) do exactly that, so the
// loop + sink live here as a template and are shared by all three formats.
//
// The byte output is identical to the pre-refactor inline loop, so markers /
// idx / page caches stay valid (no version bump).
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "HtmlStripper.h"      // HtmlStripperSink
#include "MarkerizeChapter.h"  // MarkerizeReadFn / WriteFn / AbortFn / Status / Stats

namespace snapix::smolport {

// Stripper sink that forwards emitted marker bytes to a caller's writeChunk
// callback, staging into a 1 KB buffer and flushing in bulk.  (Moved verbatim
// from MarkerizeChapter.cpp's anonymous namespace in the TXT/MD migration so
// the markerize loop template below — and markerizeChapter() — share one copy.)
//
// HtmlStripper emits plain text BYTE-BY-BYTE and markers in 2-4 byte dribbles;
// without staging a 430 KB section issued ~420 000 single-byte LittleFS writes
// (hardware repro: 116 s, ~3.7 KB/s).  Staging collapses the write-call count
// by ~3 orders of magnitude with byte-identical output.
//
// flush() MUST be called after stripper.finish() on the clean-EOF path.
// Error/abort exits skip the flush: the enclosing UnifiedCache streaming frame
// is abandoned (size never patched) so unflushed tail bytes are irrelevant.
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

// Drive an already-constructed `stripper` (which emits to `sink`) over the
// readChunk source, returning a MarkerizeStatus.  The caller owns the sink so
// it can read back the coalesced output-byte count and call flush() semantics
// match markerizeChapter().  `inBytes` / `chunkCount` are accumulated for stats.
//
// This is the loop lifted out of markerizeChapter() verbatim — same EOF/abort/
// write-error classification — so HTML/FB2 behaviour is unchanged.
template <typename Stripper>
MarkerizeStatus runMarkerizeLoop(Stripper& stripper, CallbackSink& sink,
                                 const MarkerizeReadFn& readChunk, uint8_t* chunkBuf,
                                 size_t chunkBufSize, const MarkerizeAbortFn& shouldAbort,
                                 uint32_t& inBytes, uint16_t& chunkCount) {
  for (;;) {
    if (shouldAbort && shouldAbort()) {
      return MarkerizeStatus::Aborted;
    }

    const int got = readChunk(chunkBuf, chunkBufSize);
    if (got == 0) {
      // Clean EOF — finish the stripper then drain the coalescing stage.
      stripper.finish();
      sink.flush();
      return sink.writeFailed() ? MarkerizeStatus::WriteError : MarkerizeStatus::Success;
    }
    if (got < 0) {
      return MarkerizeStatus::ReadError;
    }

    const size_t consumed = stripper.feed(chunkBuf, static_cast<size_t>(got));
    inBytes += static_cast<uint32_t>(consumed);
    ++chunkCount;

    if (sink.writeFailed()) {
      return MarkerizeStatus::WriteError;
    }

    // consumed < got iff the sink asked the stripper to stop mid-chunk
    // (abort or write-failure).  Classify accordingly.
    if (consumed < static_cast<size_t>(got)) {
      return sink.writeFailed() ? MarkerizeStatus::WriteError : MarkerizeStatus::Aborted;
    }
  }
}

}  // namespace snapix::smolport
