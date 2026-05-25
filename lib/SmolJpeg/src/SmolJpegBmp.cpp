#include "SmolJpegBmp.h"

#include <cstring>

namespace snapix::smoljpeg {

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

  // BITMAPFILEHEADER (14 bytes)
  hdr[0] = 'B'; hdr[1] = 'M';
  putLE32(&hdr[2], fileSize);
  // hdr[6..9] reserved = 0
  putLE32(&hdr[10], 62u);  // pixel data offset

  // BITMAPINFOHEADER (40 bytes), starting at offset 14
  putLE32 (&hdr[14], 40u);                                 // header size
  putLE32s(&hdr[18], static_cast<int32_t>(width));         // width (positive)
  putLE32s(&hdr[22], -static_cast<int32_t>(height));       // height NEGATIVE → top-down
  putLE16 (&hdr[26], 1u);                                  // planes
  putLE16 (&hdr[28], 1u);                                  // bpp
  putLE32 (&hdr[30], 0u);                                  // BI_RGB
  putLE32 (&hdr[34], pixelBytes);                          // image size
  putLE32 (&hdr[38], 2835u);                               // x ppm (72 dpi)
  putLE32 (&hdr[42], 2835u);                               // y ppm
  putLE32 (&hdr[46], 2u);                                  // colors used
  putLE32 (&hdr[50], 2u);                                  // important colors

  // Palette (8 bytes): index 0 = black (bit 0), index 1 = white (bit 1).
  // Matches snapix JpegToBmpConverter convention.  BGRA bytes:
  //   black  = 00 00 00 00
  //   white  = FF FF FF 00
  hdr[54] = 0x00; hdr[55] = 0x00; hdr[56] = 0x00; hdr[57] = 0x00;
  hdr[58] = 0xFF; hdr[59] = 0xFF; hdr[60] = 0xFF; hdr[61] = 0x00;

  return out.write(hdr, sizeof(hdr));
}

}  // namespace snapix::smoljpeg
