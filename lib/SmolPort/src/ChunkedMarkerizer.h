#pragma once

// =============================================================================
// ChunkedMarkerizer — markerize ONE document in source-byte chunks with a single
// HtmlStripper kept alive across them (v3.11.12, big EPUB chapters).
//
// A 418 KB single-spine EPUB chapter used to be markerized in one pass (22 s on
// device) and paginated in one walk before ANY page could show.  Writing the
// markers as consecutive chunk segments lets the page index grow chunk by
// chunk, so a saved deep page shows as soon as the walk passes it.
//
// Chunk boundaries are plain source offsets (MarkerChunkPlan) — they may fall
// inside a tag, an entity, a UTF-8 sequence or a <script> body.  That is safe
// because the SAME stripper instance consumes every byte in order: the chunk
// split only decides which segment receives the emitted bytes, so the chunk
// concatenation is byte-identical to one continuous markerizeChapter() pass
// (host-proven by EpubChunkEquivTest).  A stripper instance that is lost
// (parser reset, reboot) is rebuilt by replaying the source prefix with the
// output discarded (replayTo).
//
// The caller owns the source stream (EPUB: a ZipItemReader, reopened per call
// so the 32 KB inflate window is not pinned between calls) and positions it at
// offset() before markerizeChunk(): skipSource() for a live instance,
// replayTo() for a fresh one.
//
// v3.11.13 — the book's CSS table (if any) must be the SAME for every instance
// that markerizes one document: the stripper's element stack / open styles are
// rebuilt by replay only when the table is identical.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "HtmlStripper.h"
#include "MarkerizeChapter.h"  // MarkerizeReadFn / MarkerizeWriteFn / MarkerizeStatus

namespace snapix::smolport {

// Chunk k covers source bytes [k*chunkBytes, (k+1)*chunkBytes); chunk
// maxChunks-1 takes everything left.  A pure function of (k, source size), so a
// cold parser re-derives every chunk's range without a persisted cursor.
struct MarkerChunkPlan {
  uint32_t chunkBytes = 32768;
  int maxChunks = 240;

  uint32_t start(int k, uint32_t srcSize) const {
    const uint64_t s = static_cast<uint64_t>(k) * chunkBytes;
    return s >= srcSize ? srcSize : static_cast<uint32_t>(s);
  }
  uint32_t end(int k, uint32_t srcSize) const {
    if (k >= maxChunks - 1) return srcSize;
    const uint64_t e = static_cast<uint64_t>(k + 1) * chunkBytes;
    return e >= srcSize ? srcSize : static_cast<uint32_t>(e);
  }
  // Number of chunks the source tiles into (an empty source is one empty chunk).
  int count(uint32_t srcSize) const {
    if (srcSize == 0 || chunkBytes == 0) return 1;
    const uint64_t n = (static_cast<uint64_t>(srcSize) + chunkBytes - 1) / chunkBytes;
    return n >= static_cast<uint64_t>(maxChunks) ? maxChunks : static_cast<int>(n);
  }
};

// Read and discard `n` bytes of a sequential source.  False on a short read.
inline bool skipSource(const MarkerizeReadFn& read, uint32_t n, uint8_t* buf, size_t bufSize) {
  while (n > 0) {
    const size_t want = n < bufSize ? n : bufSize;
    const int got = read(buf, want);
    if (got <= 0) return false;
    n -= static_cast<uint32_t>(got);
  }
  return true;
}

class ChunkedMarkerizer {
 public:
  explicit ChunkedMarkerizer(HtmlStripper::Mode mode, const CssStyleTable* styles = nullptr)
      : stripper_(sink_, mode, styles) {}
  ChunkedMarkerizer(const ChunkedMarkerizer&) = delete;
  ChunkedMarkerizer& operator=(const ChunkedMarkerizer&) = delete;

  // Source bytes consumed so far (= where the next chunk starts).
  uint32_t offset() const { return offset_; }
  // False after a failed/partial chunk: the stripper consumed bytes whose
  // markers were not stored, so this instance must be discarded.
  bool clean() const { return clean_; }
  bool finished() const { return finished_; }

  // Rebuild a fresh instance's state: feed source [offset(), target) with the
  // output discarded.  The source must be positioned at offset().
  bool replayTo(const MarkerizeReadFn& read, uint32_t target, uint8_t* buf, size_t bufSize) {
    sink_.begin(nullptr, nullptr, 0);
    const bool ok = feedRange(read, target, buf, bufSize);
    sink_.end();
    if (!ok) clean_ = false;
    return ok;
  }

  // Markerize source [offset(), end) into `write`, staging output through
  // `stage` (caller-owned scratch, kept off the persistent instance).  `last`
  // = this chunk reaches the end of the document → the stripper is finished.
  // The source must be positioned at offset().  On anything but Success the
  // instance is no longer clean().
  MarkerizeStatus markerizeChunk(const MarkerizeReadFn& read, const MarkerizeWriteFn& write, uint32_t end,
                                 bool last, uint8_t* buf, size_t bufSize, uint8_t* stage, size_t stageSize,
                                 uint32_t* outBytes = nullptr) {
    sink_.begin(&write, stage, stageSize);
    bool ok = feedRange(read, end, buf, bufSize);
    if (ok && last && !finished_) {
      stripper_.finish();
      finished_ = true;
    }
    const bool flushed = sink_.flush();
    const uint32_t produced = sink_.produced();
    const bool writeFailed = sink_.writeFailed();
    sink_.end();
    if (outBytes != nullptr) *outBytes = produced;
    if (!ok || !flushed || writeFailed) {
      clean_ = false;
      return writeFailed || !flushed ? MarkerizeStatus::WriteError : MarkerizeStatus::ReadError;
    }
    return MarkerizeStatus::Success;
  }

 private:
  // Stripper sink whose target changes per chunk.  HtmlStripper emits text
  // byte-by-byte; staging collapses those into bulk writes (see CallbackSink).
  class Sink final : public HtmlStripperSink {
   public:
    void begin(const MarkerizeWriteFn* write, uint8_t* stage, size_t stageSize) {
      write_ = write;
      stage_ = stage;
      stageSize_ = stageSize;
      stageLen_ = 0;
      produced_ = 0;
      writeFailed_ = false;
    }
    void end() {
      write_ = nullptr;
      stage_ = nullptr;
      stageSize_ = 0;
      stageLen_ = 0;
    }
    void emit(const uint8_t* data, size_t len) override {
      produced_ += static_cast<uint32_t>(len);
      if (write_ == nullptr || writeFailed_) return;  // replay: discard
      if (stage_ == nullptr || len >= stageSize_) {
        if (!flush() || !(*write_)(data, len)) writeFailed_ = true;
        return;
      }
      if (stageLen_ + len > stageSize_ && !flush()) {
        writeFailed_ = true;
        return;
      }
      std::memcpy(stage_ + stageLen_, data, len);
      stageLen_ += len;
    }
    bool flush() {
      if (writeFailed_) return false;
      if (write_ == nullptr || stageLen_ == 0) return true;
      const bool ok = (*write_)(stage_, stageLen_);
      stageLen_ = 0;
      if (!ok) writeFailed_ = true;
      return ok;
    }
    bool shouldStop() const override { return writeFailed_; }
    uint32_t produced() const { return produced_; }
    bool writeFailed() const { return writeFailed_; }

   private:
    const MarkerizeWriteFn* write_ = nullptr;
    uint8_t* stage_ = nullptr;
    size_t stageSize_ = 0;
    size_t stageLen_ = 0;
    uint32_t produced_ = 0;
    bool writeFailed_ = false;
  };

  // Feed source bytes until offset_ == target.
  bool feedRange(const MarkerizeReadFn& read, uint32_t target, uint8_t* buf, size_t bufSize) {
    if (!clean_ || finished_) return offset_ == target && clean_;
    while (offset_ < target) {
      const uint32_t left = target - offset_;
      const size_t want = left < bufSize ? left : bufSize;
      const int got = read(buf, want);
      if (got <= 0) return false;
      const size_t consumed = stripper_.feed(buf, static_cast<size_t>(got));
      offset_ += static_cast<uint32_t>(consumed);
      if (consumed != static_cast<size_t>(got)) return false;  // sink write failure
    }
    return true;
  }

  Sink sink_;  // declared before stripper_: the stripper holds a reference
  HtmlStripper stripper_;
  uint32_t offset_ = 0;
  bool clean_ = true;
  bool finished_ = false;
};

}  // namespace snapix::smolport
