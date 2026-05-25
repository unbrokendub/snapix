#include "SmolPngFilter.h"

#include <cstdlib>

namespace snapix::smolpng {

namespace {

// Paeth predictor (PNG spec 9.4).  Operates on unsigned bytes promoted
// to int; the predictor's three candidates (a/b/c) are signed-equivalent.
inline int paethPredictor(const int a, const int b, const int c) {
  const int p  = a + b - c;
  const int pa = std::abs(p - a);
  const int pb = std::abs(p - b);
  const int pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc)             return b;
  return c;
}

}  // namespace

void unfilterScanline(uint8_t* curRow, const uint8_t* prevRow,
                      const size_t rowBytes, const uint8_t bpp,
                      const uint8_t filterType) {
  switch (filterType) {
    case 0:  // None — no-op.
      break;

    case 1: {  // Sub: add left neighbour (or 0 for x < bpp).
      for (size_t i = bpp; i < rowBytes; ++i) {
        curRow[i] = static_cast<uint8_t>(curRow[i] + curRow[i - bpp]);
      }
      break;
    }

    case 2: {  // Up: add byte at same x in previous row.
      for (size_t i = 0; i < rowBytes; ++i) {
        curRow[i] = static_cast<uint8_t>(curRow[i] + prevRow[i]);
      }
      break;
    }

    case 3: {  // Average: add floor((left + above) / 2).
      for (size_t i = 0; i < rowBytes; ++i) {
        const int left  = (i >= bpp) ? curRow[i - bpp] : 0;
        const int above = prevRow[i];
        curRow[i] = static_cast<uint8_t>(curRow[i] + ((left + above) >> 1));
      }
      break;
    }

    case 4: {  // Paeth.
      for (size_t i = 0; i < rowBytes; ++i) {
        const int a = (i >= bpp) ? curRow[i - bpp] : 0;         // left
        const int b = prevRow[i];                                // above
        const int c = (i >= bpp) ? prevRow[i - bpp] : 0;         // upper-left
        curRow[i] = static_cast<uint8_t>(curRow[i] + paethPredictor(a, b, c));
      }
      break;
    }

    default:
      // Treat unknown filter codes as None — keeps malformed PNGs from
      // hanging the decoder.
      break;
  }
}

}  // namespace snapix::smolpng
