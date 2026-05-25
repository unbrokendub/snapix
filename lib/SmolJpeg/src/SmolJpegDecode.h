#pragma once

// =============================================================================
// SmolJpegDecode — top-level baseline-DCT decode loop.
//
// Drives BitReader / decodeBaselineBlock / idct / ditherRowGrey across the
// MCU grid of a parsed JpegState, emitting a streaming 1-bit top-down BMP.
//
// Chrominance (Cb / Cr) blocks are Huffman-decoded to keep the bitstream in
// sync but their IDCT output is discarded — only the Y plane reaches the
// output dither stage.
//
// Port of smol-epub `decode_baseline` (jpeg.rs:730-880).
// =============================================================================

#include "SmolJpeg.h"
#include "SmolJpegState.h"

namespace snapix::smoljpeg {

// Run the entropy-coded decode against an already-parsed `state` and emit
// the full BMP stream (header + dithered rows) to `out`.  Assumes
// `state.scanStart` points to the first byte of entropy data.
//
// `maxW` / `maxH` cap the output dims; source is integer-downscaled by the
// max of (ceil(srcW/maxW), ceil(srcH/maxH), 1).
//
// `shouldAbort` (optional) is polled once per MCU row.
Status decodeBaselineStream(InputStream& in, JpegState& state, int maxW,
                            int maxH, OutputStream& out,
                            bool (*shouldAbort)());

}  // namespace snapix::smoljpeg
