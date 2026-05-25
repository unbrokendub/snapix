#pragma once

// =============================================================================
// SmolPngState — internal decoder state populated by IHDR parsing and
// used by the streaming pipeline.  Roughly mirrors SmolJpegState for
// pattern consistency.
// =============================================================================

#include "SmolPng.h"
#include "SmolPngFormat.h"

#include <cstdint>

namespace snapix::smolpng {

struct PngState {
  uint32_t  width        = 0;
  uint32_t  height       = 0;
  uint8_t   bitDepth     = 0;       // currently always 8 (MVP)
  ColorType colorType    = ColorType::Rgb;
  uint8_t   compression  = 0;       // always 0 (deflate)
  uint8_t   filterMethod = 0;       // always 0 (adaptive)
  uint8_t   interlace    = 0;       // always 0 (non-interlaced)

  // Bytes per pixel after unfiltering, derived from colorType + bitDepth.
  // Used by Sub / Average / Paeth filters and the row stride math.
  uint8_t   bytesPerPixel = 0;

  // Row stride in bytes of the decoded (unfiltered) scanline.  Equals
  // width * bytesPerPixel.  Filtered scanline is this + 1 (filter-type
  // prefix byte).
  uint32_t  rowStride = 0;

  // Where the entropy-coded data (concatenated IDAT chunks) starts —
  // computed by the chunk scanner.  IDAT chunks may be split across
  // many chunks; the decoder concatenates their payloads transparently.
  uint32_t  firstIdatOffset = 0;
};

}  // namespace snapix::smolpng
