#pragma once

#include <cstdint>

namespace snapix::smoljpeg {

// Perform an 8×8 inverse DCT on `block` (64 i32 dequantized coefficients
// in natural order) and write 8×8 grayscale output samples (0..255) to
// `out[64]` in row-major order.
//
// Algorithm: IJG ISLOW (integer slow), CONST_BITS=13.  Two passes — row
// then column.  AC=0 fast paths reduce the typical book-page JPEG to
// mostly DC-only block work.
//
// `scratch` is a 64-int32 work buffer (256 bytes) the caller pre-allocates
// once on heap and re-uses across all idct() calls in a decode pass.
// Moving this out of the function's local frame saves 256 B per call on
// the loopTask stack — critical on ESP32-C3 where the default Arduino
// loop stack is 8 KB and the reader chain already consumes ~5 KB.
//
// Direct mechanical port of smol-epub jpeg.rs::idct().  Bit-identical
// output expected.
void idct(const int32_t block[64], uint8_t out[64], int32_t scratch[64]);

}  // namespace snapix::smoljpeg
