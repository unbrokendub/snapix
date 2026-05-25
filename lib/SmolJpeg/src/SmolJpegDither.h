#pragma once

#include <cstddef>
#include <cstdint>

namespace snapix::smoljpeg {

// Floyd-Steinberg dither one row of Y (luminance) samples into a 1-bit
// packed bitmap row, MSB-first.  `bit=1` means WHITE (matches snapix's
// JpegToBmpConverter convention: palette index 0 = black, 1 = white).
//
// `row`      — source grayscale samples (1 byte per sample).
// `scale`    — integer downscale factor (sample stride).  scale=1 → use
//              every byte; scale=2 → use every 2nd byte; etc.
// `outW`     — number of output pixels for this row (width of dst image).
// `errCur`   — current-row error accumulator, size (outW + 2) int16_t.
//              Indexed by output pixel + 1 (positions 0 and outW+1 are
//              guard bytes to avoid bounds-check branches inside the
//              dither loop).  Caller manages — swap with `errNxt` between
//              rows.
// `errNxt`   — next-row error accumulator, same layout.  Cleared to 0
//              before each row's dither call.
// `outRow`   — destination 1-bit packed row, size ceil(outW/8) bytes.
//              MUST be pre-zeroed by caller (dither only sets bits, never
//              clears them).
//
// Direct mechanical port of smol-epub jpeg.rs::dither_row_grey().
void ditherRowGrey(const uint8_t* row, size_t scale, size_t outW,
                   int16_t* errCur, int16_t* errNxt, uint8_t* outRow);

}  // namespace snapix::smoljpeg
