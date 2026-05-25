#pragma once

// =============================================================================
// SmolPngDecode — top-level decoder.  Drives IdatReader → uzlib zlib
// inflate → SmolPngFilter → RGB-to-luma → SmolPngDither → 1-bit BMP
// output stream.  Streaming throughout: one scanline at a time.
// =============================================================================

#include "SmolPng.h"
#include "SmolPngState.h"

namespace snapix::smolpng {

// Decode the entire image after `parseMarkers`-style state has been
// populated.  `state.firstIdatOffset` must point at the first IDAT
// chunk header.  Writes the full BMP file (header + dithered pixel data)
// to `out`.  Returns Status::Ok on success.
Status decodeBaselineStream(InputStream& in, PngState& state, int maxW,
                            int maxH, OutputStream& out,
                            bool (*shouldAbort)());

}  // namespace snapix::smolpng
