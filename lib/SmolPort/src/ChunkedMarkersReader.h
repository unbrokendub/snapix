#pragma once

// =============================================================================
// ChunkedMarkersReader — present a set of marker CHUNK segments as ONE logical
// concatenated byte stream (global offsets), so a single-section document's
// markers can be stored progressively across several UnifiedCache segments
// (lazy markerize) while the page idx + render still see one continuous stream.
//
// Why: UnifiedCache writes a segment atomically (one blocking call), so a big
// single-section doc (TXT/MD) can't surface page 0 until the whole file is
// markerized.  Markerizing in line-bounded CHUNKS (one segment each, key =
// chunk index) lets page 0 render after chunk 0, with the rest filling in the
// background.  This reader hides the chunk boundaries: the idx stores GLOBAL
// byteOffsets into the concatenation, and renderMarkerizedPage / the MEASURE
// walk are unchanged — they just read through this instead of a single segment.
//
// A 1-chunk document (small TXT/MD, or any EPUB/FB2 section) behaves exactly
// like the old single-segment reader, so existing behaviour is preserved.
//
// The reader is decoupled from UnifiedCache via MarkerChunkProvider so the
// span/seek logic is host-testable with in-memory chunks.
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace snapix::smolport {

// Random-access-by-chunk byte source.  `open(k)` makes chunk k current and
// positions it at byte 0; `read`/`seekLocal` operate on the current chunk.
class MarkerChunkProvider {
 public:
  virtual ~MarkerChunkProvider() = default;
  virtual int count() const = 0;            // number of available chunks (>= 0)
  virtual uint32_t size(int chunk) const = 0;  // payload bytes in `chunk`
  virtual bool open(int chunk) = 0;         // make `chunk` current, seek to 0
  virtual int read(uint8_t* buf, size_t bufSize) = 0;  // read from current chunk; 0 = chunk EOF, <0 = error
  virtual bool seekLocal(uint32_t offset) = 0;         // seek within current chunk
};

// Presents the provider's chunks as one stream.  Construct, optionally
// seekGlobal() to a resume offset, then read() sequentially (spans chunks).
class ChunkedMarkersReader {
 public:
  explicit ChunkedMarkersReader(MarkerChunkProvider& provider) : provider_(provider) {
    chunkCount_ = provider_.count();
    total_ = 0;
    for (int i = 0; i < chunkCount_; ++i) total_ += provider_.size(i);
  }

  // Total bytes across all chunks (the logical stream length).
  uint32_t totalSize() const { return total_; }
  int chunkCount() const { return chunkCount_; }

  // Position the stream at a global byte offset.  Opens the containing chunk
  // and seeks within it.  Returns false if offset > totalSize or on I/O error.
  bool seekGlobal(uint32_t globalOffset) {
    if (globalOffset > total_) return false;
    uint32_t base = 0;
    for (int i = 0; i < chunkCount_; ++i) {
      const uint32_t sz = provider_.size(i);
      if (globalOffset < base + sz || i == chunkCount_ - 1) {
        if (!provider_.open(i)) return false;
        if (!provider_.seekLocal(globalOffset - base)) return false;
        curChunk_ = i;
        opened_ = true;
        return true;
      }
      base += sz;
    }
    // Empty stream (no chunks) — only valid if offset is 0.
    return globalOffset == 0;
  }

  // Sequential read across chunk boundaries.  Returns bytes read into `buf`
  // (> 0), 0 at end of the last chunk (logical EOF), or < 0 on error.  Lazily
  // opens chunk 0 on first call if seekGlobal() wasn't used.
  int read(uint8_t* buf, size_t bufSize) {
    if (chunkCount_ <= 0) return 0;
    if (!opened_) {
      if (!provider_.open(0)) return -1;
      curChunk_ = 0;
      opened_ = true;
    }
    for (;;) {
      const int n = provider_.read(buf, bufSize);
      if (n < 0) return -1;
      if (n > 0) return n;
      // Current chunk exhausted — advance to the next, if any.
      if (curChunk_ + 1 >= chunkCount_) return 0;  // logical EOF
      if (!provider_.open(curChunk_ + 1)) return -1;
      ++curChunk_;
      // loop: read from the freshly-opened next chunk
    }
  }

 private:
  MarkerChunkProvider& provider_;
  int chunkCount_ = 0;
  uint32_t total_ = 0;
  int curChunk_ = -1;
  bool opened_ = false;
};

}  // namespace snapix::smolport
