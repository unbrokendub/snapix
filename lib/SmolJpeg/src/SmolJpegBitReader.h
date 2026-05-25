#pragma once

// =============================================================================
// SmolJpegBitReader — MSB-first bit-level reader over an InputStream, with
// JPEG byte-stuffing semantics:
//
//   * Data byte 0xFF is encoded as the byte pair (0xFF, 0x00) in the stream.
//   * Any other byte after 0xFF is a JPEG marker (RSTn, EOI, etc).  Marker
//     consumption is the caller's responsibility — when one is encountered
//     mid-bitstream, `pendingMarker()` returns the marker byte and `ensure()`
//     stops returning fresh data bits (it pads with 1-bits up to the
//     requested count so canonical Huffman decode terminates on an invalid
//     code rather than hanging).
//   * 0xFF padding (multiple 0xFF bytes before a marker) is collapsed.
//
// Port of smol-epub `BitReader` (jpeg.rs:240-285).
// =============================================================================

#include "SmolJpeg.h"

#include <cstddef>
#include <cstdint>

namespace snapix::smoljpeg {

class BitReader {
 public:
  // Read MSB-first bits from `in` between absolute offsets
  // [startOffset, endOffset).  Caller owns `in` for the BitReader's lifetime.
  BitReader(InputStream& in, uint32_t startOffset, uint32_t endOffset);

  // Pull bytes from the stream until at least `n` bits are buffered, OR
  // a marker is hit / stream exhausted.  After marker hit, pads with
  // 1-bits up to `n` so downstream Huffman decode fails cleanly.
  // Returns true if bitCount_ >= n on exit.
  bool ensure(uint32_t n);

  // Read `n` MSB-first bits.  1 ≤ n ≤ 16.  Sets status_ = InvalidJpeg
  // on underflow (returns 0 in that case).
  uint32_t readBits(uint32_t n);

  // Peek top `n` bits without consuming.  Same range as readBits.
  uint32_t peekBits(uint32_t n);

  // Drop `n` bits (caller must have already ensured them).
  void consumeBits(uint32_t n);

  // Currently-buffered bit count (0..24).  Mostly for tests.
  uint32_t bitCount() const { return bitCount_; }

  // Align to next byte boundary (drops fractional bits).  Used before
  // dispatching a restart marker.
  void alignToByte();

  // Pending marker byte (0 if none).  Set when the input bytestream
  // signaled a JPEG marker (e.g. RSTn, EOI).
  uint8_t pendingMarker() const { return marker_; }

  // Reset pending marker after caller has dispatched it.  Useful before
  // resuming entropy-coded data after RSTn.
  void clearMarker() { marker_ = 0; }

  // Absolute stream offset of the next byte the BitReader will pull.
  // Reflects already-consumed buffer bytes; bitBuf_ may still hold bits
  // from earlier bytes.
  uint32_t streamOffset() const { return bufStart_ + bufPos_; }

  Status status() const { return status_; }

 private:
  // Pull one logical data byte (handling stuffing/markers).
  // Returns 0..255 on success, -1 if marker hit / EOS.
  int nextDataByte();

  // Refill internal buf_ from the InputStream.  Returns true if any
  // bytes loaded.
  bool refillBuffer();

  InputStream& in_;
  uint32_t endOffset_;
  uint32_t bufStart_;
  uint16_t bufPos_;
  uint16_t bufLen_;
  uint8_t  buf_[256];
  uint32_t bitBuf_;
  uint32_t bitCount_;
  uint8_t  marker_;
  Status   status_;
};

}  // namespace snapix::smoljpeg
