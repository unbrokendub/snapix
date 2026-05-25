#pragma once

// =============================================================================
// SmolPngChunk — chunk header reader + IHDR parser.
//
// Provides the byte-level entry points used by both `peekDimensions` and
// the full decoder.  CRC validation is NOT performed here (Phase 6b will
// add it as an opt-in cost path); we trust the underlying file system.
// =============================================================================

#include "SmolPng.h"
#include "SmolPngFormat.h"
#include "SmolPngState.h"

#include <cstddef>
#include <cstdint>

namespace snapix::smolpng {

struct ChunkHeader {
  uint32_t length;   // payload bytes (not including length/type/CRC)
  uint32_t type;     // 4-byte FourCC packed big-endian via fourcc()
  uint32_t payloadOffset;  // absolute file offset to payload start
};

// Verify the 8-byte PNG signature at the start of `in`.  Returns true on
// match.  Reads exactly 8 bytes.
bool readSignature(InputStream& in);

// Read the chunk header at absolute file offset `offset`.  Populates
// `out` (type, length, payload offset).  Returns true on success;
// false on short read.  Does NOT validate length against file size.
bool readChunkHeader(InputStream& in, uint32_t offset, ChunkHeader& out);

// Parse an IHDR chunk's 13-byte payload at `payloadOffset` into `state`.
// Validates the MVP-supported feature set:
//   * width / height must be > 0 and ≤ kMaxPixels combined
//   * bit depth == 8
//   * color type == Rgb (2) or RgbA (6)
//   * compression == 0
//   * filter method == 0
//   * interlace == 0
// Returns Status::Ok on success.  Other Status values indicate which
// rule was violated.
Status parseIhdr(InputStream& in, uint32_t payloadOffset, PngState& state);

// Scan chunks from the byte right after the signature through to the
// first IDAT.  Populates `state` (via parseIhdr on IHDR), sets
// `state.firstIdatOffset` to the absolute file offset of the first
// IDAT's chunk header, and returns Status::Ok.
//
// Tolerates and skips ancillary chunks (gAMA, sRGB, tEXt, pHYs, etc.)
// between IHDR and the first IDAT.  Returns InvalidPng if IHDR is
// not the first chunk, or if EOI hit before any IDAT.
Status scanThroughIdat(InputStream& in, PngState& state);

}  // namespace snapix::smolpng
