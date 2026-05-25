#pragma once

// =============================================================================
// SmolPngBmp — 1-bit BMP header writer.  Duplicate of SmolJpegBmp with the
// same layout, palette convention (index 0 = black, 1 = white), and top-down
// orientation.
// =============================================================================

#include "SmolPng.h"

#include <cstddef>
#include <cstdint>

namespace snapix::smolpng {

constexpr uint32_t bmp1BitRowStride(const uint32_t width) {
  return ((width + 31u) / 32u) * 4u;
}

bool writeBmp1BitHeader(OutputStream& out, uint32_t width, uint32_t height);

}  // namespace snapix::smolpng
