#include "SmolPngDither.h"

namespace snapix::smolpng {

void ditherRowGrey(const uint8_t* row, const size_t scale, const size_t outW,
                   int16_t* errCur, int16_t* errNxt, uint8_t* outRow) {
  for (size_t ox = 0; ox < outW; ++ox) {
    const size_t sx = ox * scale;
    const int16_t g = static_cast<int16_t>(row[sx]);
    int16_t val = static_cast<int16_t>(g + errCur[ox + 1]);
    if (val < 0) val = 0;
    if (val > 255) val = 255;

    // Snapix 1-bit convention: bit=1 → white, bit=0 → black.
    const bool white = val >= 128;
    const int16_t q = white ? 255 : 0;
    const int16_t e = static_cast<int16_t>(val - q);

    if (white) {
      outRow[ox / 8] |= static_cast<uint8_t>(1u << (7 - (ox & 7)));
    }

    // Floyd-Steinberg error distribution:
    //         X    7/16
    //  3/16  5/16  1/16
    errCur[ox + 2] = static_cast<int16_t>(errCur[ox + 2] + e * 7 / 16);
    errNxt[ox]     = static_cast<int16_t>(errNxt[ox]     + e * 3 / 16);
    errNxt[ox + 1] = static_cast<int16_t>(errNxt[ox + 1] + e * 5 / 16);
    errNxt[ox + 2] = static_cast<int16_t>(errNxt[ox + 2] + e / 16);
  }
}

}  // namespace snapix::smolpng
