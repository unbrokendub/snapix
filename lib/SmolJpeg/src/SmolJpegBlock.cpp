#include "SmolJpegBlock.h"

#include <cstring>

namespace snapix::smoljpeg {

Status decodeBaselineBlock(BitReader& br, const HuffTable& dcTbl,
                           const HuffTable& acTbl, const uint16_t qt[64],
                           int32_t& predictor, int32_t out[64],
                           int32_t scratchZigzag[64]) {
  // 64 coefficients in scan (zig-zag) order — cleared to 0 by default.
  // `scratchZigzag` is caller-owned heap memory; we reuse the name `coef`
  // locally to keep the body code identical to the smol-epub reference.
  int32_t* const coef = scratchZigzag;
  std::memset(coef, 0, 64 * sizeof(int32_t));

  // ---- DC ----
  const int catDC = decodeHuffmanSymbol(br, dcTbl);
  if (catDC < 0 || catDC > 11) return Status::InvalidJpeg;

  int32_t diff = 0;
  if (catDC > 0) {
    if (!br.ensure(static_cast<uint32_t>(catDC))) return Status::InvalidJpeg;
    diff = extendSign(br.readBits(static_cast<uint32_t>(catDC)),
                      static_cast<uint32_t>(catDC));
  }
  predictor += diff;
  coef[0] = predictor;

  // ---- AC: run-length (run, size) until EOB or 63 coefficients reached ----
  int k = 1;
  while (k < 64) {
    const int rs = decodeHuffmanSymbol(br, acTbl);
    if (rs < 0) return Status::InvalidJpeg;

    const int run  = rs >> 4;
    const int size = rs & 0x0F;

    if (size == 0) {
      if (run == 15) {
        // ZRL: skip 16 zeros and continue.
        k += 16;
        if (k > 64) return Status::InvalidJpeg;
        continue;
      }
      // EOB: remaining coefficients are zero (already zero-init'd).
      break;
    }

    k += run;
    if (k >= 64) return Status::InvalidJpeg;

    if (!br.ensure(static_cast<uint32_t>(size))) return Status::InvalidJpeg;
    coef[k] = extendSign(br.readBits(static_cast<uint32_t>(size)),
                         static_cast<uint32_t>(size));
    ++k;
  }

  // Apply zig-zag inverse permutation + dequantization in one pass.
  for (int i = 0; i < 64; ++i) {
    out[kZigZag[i]] = coef[i] * static_cast<int32_t>(qt[i]);
  }

  return Status::Ok;
}

}  // namespace snapix::smoljpeg
