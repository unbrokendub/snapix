#include "SmolJpegIDCT.h"

#include "SmolJpegState.h"

namespace snapix::smoljpeg {

namespace {

// (x + (1 << (n-1))) >> n — rounding right shift used throughout IDCT.
inline int32_t descale(const int32_t x, const int32_t n) {
  return (x + (int32_t{1} << (n - 1))) >> n;
}

// Saturating cast to byte (0..255).
inline uint8_t clamp255(const int32_t x) {
  if (x < 0) return 0;
  if (x > 255) return 255;
  return static_cast<uint8_t>(x);
}

}  // namespace

// Direct port of smol-epub jpeg.rs::idct().  Wrapping multiplications
// preserved (Rust `wrapping_mul` → C++ unsigned arithmetic via static_cast
// would be needed for strict equivalence, but in practice JPEG coefficients
// never overflow i32 with these constants).  We keep signed arithmetic
// for readability; the input values are bounded by IDCT range.
void idct(const int32_t block[64], uint8_t out[64], int32_t scratch[64]) {
  int32_t* const ws = scratch;

  // ---- row pass ----------------------------------------------------------
  for (int row = 0; row < 8; ++row) {
    const int b = row * 8;
    const int32_t d0 = block[b], d1 = block[b + 1], d2 = block[b + 2], d3 = block[b + 3];
    const int32_t d4 = block[b + 4], d5 = block[b + 5], d6 = block[b + 6], d7 = block[b + 7];

    // Fast path: all AC zero → DC-only row.
    if (d1 == 0 && d2 == 0 && d3 == 0 && d4 == 0 && d5 == 0 && d6 == 0 && d7 == 0) {
      const int32_t dc = d0 << kP1;
      for (int i = 0; i < 8; ++i) {
        ws[b + i] = dc;
      }
      continue;
    }

    const int32_t z1 = (d2 + d6) * kF0541;
    const int32_t tmp2 = z1 + d6 * (-kF1847);
    const int32_t tmp3 = z1 + d2 * kF0765;
    const int32_t tmp0 = (d0 + d4) << kCB;
    const int32_t tmp1 = (d0 - d4) << kCB;
    const int32_t t10 = tmp0 + tmp3;
    const int32_t t13 = tmp0 - tmp3;
    const int32_t t11 = tmp1 + tmp2;
    const int32_t t12 = tmp1 - tmp2;

    const int32_t zz1 = d7 + d1;
    const int32_t zz2 = d5 + d3;
    const int32_t zz3 = d7 + d3;
    const int32_t zz4 = d5 + d1;
    const int32_t z5 = (zz3 + zz4) * kF1175;

    int32_t o0 = d7 * kF0298;
    int32_t o1 = d5 * kF2053;
    int32_t o2 = d3 * kF3072;
    int32_t o3 = d1 * kF1501;
    const int32_t s1 = zz1 * (-kF0899);
    const int32_t s2 = zz2 * (-kF2562);
    const int32_t s3 = zz3 * (-kF1961) + z5;
    const int32_t s4 = zz4 * (-kF0390) + z5;
    o0 += s1 + s3;
    o1 += s2 + s4;
    o2 += s2 + s3;
    o3 += s1 + s4;

    const int32_t sh = kCB - kP1;
    ws[b]     = descale(t10 + o3, sh);
    ws[b + 7] = descale(t10 - o3, sh);
    ws[b + 1] = descale(t11 + o2, sh);
    ws[b + 6] = descale(t11 - o2, sh);
    ws[b + 2] = descale(t12 + o1, sh);
    ws[b + 5] = descale(t12 - o1, sh);
    ws[b + 3] = descale(t13 + o0, sh);
    ws[b + 4] = descale(t13 - o0, sh);
  }

  // ---- column pass -------------------------------------------------------
  for (int col = 0; col < 8; ++col) {
    const int32_t d0 = ws[col], d1 = ws[col + 8], d2 = ws[col + 16], d3 = ws[col + 24];
    const int32_t d4 = ws[col + 32], d5 = ws[col + 40], d6 = ws[col + 48], d7 = ws[col + 56];

    // Fast path: all AC zero → DC-only column.
    if (d1 == 0 && d2 == 0 && d3 == 0 && d4 == 0 && d5 == 0 && d6 == 0 && d7 == 0) {
      const uint8_t v = clamp255(descale(d0, kP1 + 3) + 128);
      out[col]      = v;
      out[col + 8]  = v;
      out[col + 16] = v;
      out[col + 24] = v;
      out[col + 32] = v;
      out[col + 40] = v;
      out[col + 48] = v;
      out[col + 56] = v;
      continue;
    }

    const int32_t z1 = (d2 + d6) * kF0541;
    const int32_t tmp2 = z1 + d6 * (-kF1847);
    const int32_t tmp3 = z1 + d2 * kF0765;
    const int32_t tmp0 = (d0 + d4) << kCB;
    const int32_t tmp1 = (d0 - d4) << kCB;
    const int32_t t10 = tmp0 + tmp3;
    const int32_t t13 = tmp0 - tmp3;
    const int32_t t11 = tmp1 + tmp2;
    const int32_t t12 = tmp1 - tmp2;

    const int32_t zz1 = d7 + d1;
    const int32_t zz2 = d5 + d3;
    const int32_t zz3 = d7 + d3;
    const int32_t zz4 = d5 + d1;
    const int32_t z5 = (zz3 + zz4) * kF1175;

    int32_t o0 = d7 * kF0298;
    int32_t o1 = d5 * kF2053;
    int32_t o2 = d3 * kF3072;
    int32_t o3 = d1 * kF1501;
    const int32_t s1 = zz1 * (-kF0899);
    const int32_t s2 = zz2 * (-kF2562);
    const int32_t s3 = zz3 * (-kF1961) + z5;
    const int32_t s4 = zz4 * (-kF0390) + z5;
    o0 += s1 + s3;
    o1 += s2 + s4;
    o2 += s2 + s3;
    o3 += s1 + s4;

    const int32_t sh = kCB + kP1 + 3;
    out[col]      = clamp255(descale(t10 + o3, sh) + 128);
    out[col + 56] = clamp255(descale(t10 - o3, sh) + 128);
    out[col + 8]  = clamp255(descale(t11 + o2, sh) + 128);
    out[col + 48] = clamp255(descale(t11 - o2, sh) + 128);
    out[col + 16] = clamp255(descale(t12 + o1, sh) + 128);
    out[col + 40] = clamp255(descale(t12 - o1, sh) + 128);
    out[col + 24] = clamp255(descale(t13 + o0, sh) + 128);
    out[col + 32] = clamp255(descale(t13 - o0, sh) + 128);
  }
}

}  // namespace snapix::smoljpeg
