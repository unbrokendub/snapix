#pragma once

// =============================================================================
// SmolJpegBmp — minimal 1-bit-per-pixel BMP writer.
//
// File layout (62 bytes header + pixel data):
//   14 B BITMAPFILEHEADER ("BM", file size, reserved, pixel offset = 62)
//   40 B BITMAPINFOHEADER (size=40, w, -h, planes=1, bpp=1, BI_RGB, ...)
//    8 B 2-color palette  (black = bit-0, white = bit-1) — matches
//                          snapix JpegToBmpConverter convention.
//   --   pixel rows, each padded to 4-byte boundary, top-down (h < 0)
// =============================================================================

#include "SmolJpeg.h"

#include <cstddef>
#include <cstdint>

namespace snapix::smoljpeg {

// Bytes per row of 1-bit pixel data, rounded up to 4-byte boundary.
constexpr uint32_t bmp1BitRowStride(const uint32_t width) {
  return ((width + 31u) / 32u) * 4u;
}

// Write a 62-byte top-down 1-bit BMP header for an image of (width × height)
// pixels.  Returns false if `out.write()` failed.
bool writeBmp1BitHeader(OutputStream& out, uint32_t width, uint32_t height);

}  // namespace snapix::smoljpeg
