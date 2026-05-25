#pragma once

// =============================================================================
// SmolJpegBlock — decode one 8×8 baseline-DCT coefficient block from a
// BitReader, then dequantize and apply inverse zig-zag into natural order.
//
// Port of smol-epub `decode_block` (jpeg.rs:455-510).  Output is ready to
// feed directly into `idct()`.
// =============================================================================

#include "SmolJpeg.h"
#include "SmolJpegBitReader.h"
#include "SmolJpegHuffman.h"
#include "SmolJpegState.h"

#include <cstdint>

namespace snapix::smoljpeg {

// Sign-extend a `category`-bit value read from a BitReader, per JPEG
// signed-magnitude convention.  E.g.:
//   cat=3, v=0b011  →  +3
//   cat=3, v=0b000  →  -7
//   cat=3, v=0b111  →  +7
//   cat=3, v=0b100  →  +4   (because 100 >= 100 → positive as-is)
//   cat=3, v=0b001  →  -6   (because 001 < 100  → negative, v - (2^3 - 1))
inline int32_t extendSign(const uint32_t v, const uint32_t category) {
  if (category == 0) return 0;
  const int32_t vi = static_cast<int32_t>(v);
  const int32_t threshold = int32_t{1} << (category - 1);
  if (vi < threshold) {
    return vi - ((int32_t{1} << category) - 1);
  }
  return vi;
}

// Decode one 8×8 DCT block.  On entry, `*predictor` holds the running DC
// value for this component; on exit it is updated.  `out` is filled with
// 64 dequantized coefficients in natural row-major order (zig-zag applied).
//
// `qt` is the 64-entry quantization table for this component, stored in
// zig-zag order (same order coefficients arrive on the wire).
//
// `scratchZigzag` is a 64-int32 work buffer (256 bytes) the caller
// pre-allocates once on heap and re-uses across every block in a decode
// pass.  Holds coefficients in their on-wire zig-zag order before the
// inverse permutation writes into `out`.  Moving out of this function's
// frame saves 256 B of stack per call on hot paths — see SmolJpegIDCT.h.
//
// Returns Status::Ok on success, or InvalidJpeg if the bitstream is
// malformed (bad Huffman codes, AC index overflow, DC category > 11).
Status decodeBaselineBlock(BitReader& br, const HuffTable& dcTbl,
                           const HuffTable& acTbl, const uint16_t qt[64],
                           int32_t& predictor, int32_t out[64],
                           int32_t scratchZigzag[64]);

}  // namespace snapix::smoljpeg
