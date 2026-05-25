#pragma once

// =============================================================================
// SmolPngIdatReader — chunk-stitching byte reader over consecutive IDAT
// chunks.  Presents one continuous byte stream to uzlib's deflate decoder,
// transparently skipping chunk headers (length + type) and trailing CRCs.
//
// PNG image data is stored across one or more IDAT chunks back-to-back;
// the concatenated payloads form a single zlib-wrapped deflate stream.
// We read 8-byte chunk headers and 4-byte CRCs out of band, and feed
// only the payload bytes to uzlib.
//
// Buffering: 128-byte internal buffer avoids one InputStream::read() per
// byte — significant on SD-card reads (~300 µs latency each).
// =============================================================================

#include "SmolPng.h"

#include <cstddef>
#include <cstdint>

namespace snapix::smolpng {

class IdatReader {
 public:
  // `firstIdatHeaderOffset` is the absolute file offset of the first
  // IDAT chunk's header (4-byte length + 4-byte "IDAT").
  IdatReader(InputStream& in, uint32_t firstIdatHeaderOffset);

  // Read the next data byte from the concatenated IDAT payloads.  Returns
  // 0..255 on success, -1 on EOF (IEND reached or stream truncated).
  int nextByte();

  // True once nextByte() has returned -1; further calls also return -1.
  bool eof() const { return eof_; }

 private:
  // Advance to the next IDAT chunk after `nextChunkHeaderOffset_`.  Skips
  // ancillary chunks transparently.  Returns true if an IDAT was found.
  bool seekNextIdat();

  // Refill the internal byte buffer from the current chunk.  Returns
  // false on read error or chunk exhaustion.
  bool refillBuffer();

  InputStream& in_;
  uint32_t     nextChunkHeaderOffset_;   // where to look for the next chunk
  uint32_t     curChunkPayloadCursor_;   // absolute offset, next byte to read
  uint32_t     curChunkRemaining_;       // bytes left in current chunk
  bool         eof_;

  static constexpr uint32_t kBufSize = 128;
  uint8_t  buf_[kBufSize];
  uint32_t bufLen_;
  uint32_t bufPos_;
};

}  // namespace snapix::smolpng
