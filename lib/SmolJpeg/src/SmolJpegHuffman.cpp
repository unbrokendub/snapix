#include "SmolJpegHuffman.h"

#include <cstring>

namespace snapix::smoljpeg {

bool buildHuffTable(HuffTable& tbl, const uint8_t bits[16],
                    const uint8_t* values, const uint16_t nValues) {
  std::memset(&tbl, 0, sizeof(tbl));
  // Default maxcode to -1 (no code of this length).
  for (int L = 0; L <= 16; ++L) tbl.maxcode[L] = -1;

  if (nValues > 256) return false;

  // Step 1 (JPEG spec C.2): build sequence of code lengths.
  uint8_t huffsize[257];
  uint16_t k = 0;
  for (int L = 1; L <= 16; ++L) {
    uint8_t n = bits[L - 1];
    while (n--) {
      if (k >= 256) return false;
      huffsize[k++] = static_cast<uint8_t>(L);
    }
  }
  if (k != nValues) return false;
  if (k == 0) return true;  // empty table is technically legal
  huffsize[k] = 0;

  // Step 2 (JPEG spec C.3): assign canonical codes.
  uint16_t huffcode[256];
  uint32_t code = 0;
  uint8_t  si = huffsize[0];
  for (uint16_t i = 0; i < k; ++i) {
    while (huffsize[i] != si) {
      code <<= 1;
      si++;
    }
    if (code > 0xFFFFu) return false;  // overflow → malformed table
    huffcode[i] = static_cast<uint16_t>(code);
    code++;
  }

  // Step 3: per-length mincode/maxcode/valptr for slow-path decode.
  uint16_t j = 0;
  for (int L = 1; L <= 16; ++L) {
    if (bits[L - 1] == 0) {
      tbl.maxcode[L] = -1;
    } else {
      tbl.valptr[L]  = static_cast<int32_t>(j);
      tbl.mincode[L] = huffcode[j];
      j += bits[L - 1];
      tbl.maxcode[L] = huffcode[j - 1];
    }
  }

  // Step 4: populate 8-bit prefix LUT for fast-path decode (codes ≤ 8 bits).
  for (uint16_t i = 0; i < k; ++i) {
    const uint8_t L = huffsize[i];
    if (L > 8) continue;
    const uint32_t shift = 8u - L;
    const uint32_t base  = static_cast<uint32_t>(huffcode[i]) << shift;
    const uint32_t fill  = 1u << shift;
    for (uint32_t p = 0; p < fill; ++p) {
      tbl.lutBits[base + p] = L;
      tbl.lutSym[base + p]  = values[i];
    }
  }

  // Copy values for slow-path symbol lookup.
  std::memcpy(tbl.values, values, nValues);
  return true;
}

int decodeHuffmanSymbol(BitReader& br, const HuffTable& tbl) {
  if (!br.ensure(8)) return -1;

  const uint32_t peek = br.peekBits(8);
  const uint8_t L = tbl.lutBits[peek];
  if (L != 0) {
    br.consumeBits(L);
    return static_cast<int>(tbl.lutSym[peek]);
  }

  // Slow path: codes of length 9..16.  Consume the 8-bit peek, then
  // append bits until match.
  br.consumeBits(8);
  int32_t code = static_cast<int32_t>(peek);
  for (int len = 9; len <= 16; ++len) {
    if (!br.ensure(1)) return -1;
    code = (code << 1) | static_cast<int32_t>(br.readBits(1));
    if (code <= tbl.maxcode[len]) {
      const int32_t idx = tbl.valptr[len] + (code - tbl.mincode[len]);
      if (idx < 0 || idx >= 256) return -1;
      return static_cast<int>(tbl.values[idx]);
    }
  }
  return -1;  // no match within 16 bits → corrupt stream
}

}  // namespace snapix::smoljpeg
