#include "SmolPngBmp.h"

#include <cstring>

namespace snapix::smolpng {

namespace {

inline void putLE16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}
inline void putLE32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}
inline void putLE32s(uint8_t* p, int32_t v) { putLE32(p, static_cast<uint32_t>(v)); }

}  // namespace

bool writeBmp1BitHeader(OutputStream& out, const uint32_t width,
                        const uint32_t height) {
  const uint32_t rowStride = bmp1BitRowStride(width);
  const uint32_t pixelBytes = rowStride * height;
  const uint32_t fileSize   = 62u + pixelBytes;

  uint8_t hdr[62];
  std::memset(hdr, 0, sizeof(hdr));

  hdr[0] = 'B'; hdr[1] = 'M';
  putLE32(&hdr[2], fileSize);
  putLE32(&hdr[10], 62u);

  putLE32 (&hdr[14], 40u);
  putLE32s(&hdr[18], static_cast<int32_t>(width));
  putLE32s(&hdr[22], -static_cast<int32_t>(height));  // top-down
  putLE16 (&hdr[26], 1u);
  putLE16 (&hdr[28], 1u);
  putLE32 (&hdr[30], 0u);
  putLE32 (&hdr[34], pixelBytes);
  putLE32 (&hdr[38], 2835u);
  putLE32 (&hdr[42], 2835u);
  putLE32 (&hdr[46], 2u);
  putLE32 (&hdr[50], 2u);

  // Palette: snapix convention — index 0 = black, 1 = white.
  hdr[54] = 0x00; hdr[55] = 0x00; hdr[56] = 0x00; hdr[57] = 0x00;
  hdr[58] = 0xFF; hdr[59] = 0xFF; hdr[60] = 0xFF; hdr[61] = 0x00;

  return out.write(hdr, sizeof(hdr));
}

}  // namespace snapix::smolpng
