#pragma once

// =============================================================================
// SmolPngFilter — invert one of the five PNG scanline filters in place.
//
// PNG spec section 9 describes filter types:
//   0  None     — bytes pass through
//   1  Sub      — current byte += previous byte in same row (offset by bpp)
//   2  Up       — current byte += byte at same x in previous row
//   3  Average  — current byte += floor((left + above) / 2)
//   4  Paeth    — current byte += Paeth predictor of (left, above, upper-left)
//
// "Previous row" for the first scanline is implicitly zero — caller should
// pass a zero-filled `prevRow` buffer for the first call.
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace snapix::smolpng {

// Invert a PNG scanline filter in place.  `curRow` is the filtered scanline
// data (NOT including the leading filter-type byte).  `prevRow` is the
// unfiltered data of the previous scanline (also without filter byte);
// pass an all-zero buffer for the first scanline.
//
// `rowBytes` is the byte length of one unfiltered scanline = width * bpp.
// `bpp` is the byte stride to the "left" pixel (3 for RGB, 4 for RGBA).
// `filterType` is the byte from the scanline header (0..4); any other
// value is treated as type 0 (defensive).
void unfilterScanline(uint8_t* curRow, const uint8_t* prevRow,
                      size_t rowBytes, uint8_t bpp, uint8_t filterType);

}  // namespace snapix::smolpng
