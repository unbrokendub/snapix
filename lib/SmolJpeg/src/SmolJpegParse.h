#pragma once

// =============================================================================
// SmolJpegParse — JPEG marker-segment parser.
//
// Reads from an InputStream and populates `JpegState` with frame dimensions,
// quantization tables, Huffman tables, restart interval, and scan parameters.
// Stops at the SOS marker; entropy-coded data starts at `state.scanStart`.
//
// Port of smol-epub `parse_markers` / `parse_*` (jpeg.rs:529-720).
//
// Two entry points:
//   parseMarkers          — full parse: SOI through SOS, populates all of
//                           JpegState.  Streams directly from the input
//                           (256-byte sliding window — no large heap
//                           scratch buffer).
//   scanSofDimensions     — light-weight: returns just width/height from
//                           the first SOF segment found.  Used by
//                           peekDimensions() — no full state needed.
//
// v2.0.88 hardware-bring-up note: the original implementation pre-read
// up to 32 KB of the JPEG into a heap scratch buffer and parsed from
// there.  That spike failed `AllocFailed` under heap pressure when
// invoked from the cover-gen call chain (heap 90 KB free, largest free
// block <32 KB after prior allocations).  The streaming rewrite caps
// transient parse RAM at <300 bytes (a 256-byte sliding window plus a
// handful of locals), and the old `kHeaderReadBytes` constant was
// retired alongside it.
// =============================================================================

#include "SmolJpeg.h"
#include "SmolJpegState.h"

#include <cstdint>

namespace snapix::smoljpeg {

// Full marker-segment parse.  On success, `state` is populated and
// `state.scanStart` points to the first byte of entropy-coded data.
// `state` may be passed in any state — parseMarkers zeros it first.
Status parseMarkers(InputStream& in, JpegState& state);

// Fast-path: find the first SOF0/SOF2 marker and return its dimensions.
// Does not validate the rest of the header.  No persistent state needed.
Status scanSofDimensions(InputStream& in, uint16_t& outWidth,
                         uint16_t& outHeight);

}  // namespace snapix::smoljpeg
