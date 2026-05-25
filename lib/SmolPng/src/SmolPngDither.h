#pragma once

// =============================================================================
// SmolPngDither — Floyd-Steinberg row dither.  Same algorithm and bit
// convention as snapix::smoljpeg::ditherRowGrey (intentionally duplicated
// to keep SmolPng standalone — no cross-lib dep).  See SmolJpegDither.h
// for the protocol explanation.
//
// Bit convention: bit=1 → white (matches snapix JpegToBmpConverter
// convention; palette index 0 = black, 1 = white).
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace snapix::smolpng {

void ditherRowGrey(const uint8_t* row, size_t scale, size_t outW,
                   int16_t* errCur, int16_t* errNxt, uint8_t* outRow);

}  // namespace snapix::smolpng
