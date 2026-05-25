#pragma once

// =============================================================================
// SmolPngFormat — PNG file format constants and chunk-type helpers.
//
// Spec: https://www.w3.org/TR/png/ (PNG 2nd Edition).  Key invariants:
//   * Big-endian throughout (4-byte length, 4-byte CRC).
//   * Chunks: [length:u32 BE][type:4 ASCII][data:length bytes][CRC32:u32 BE].
//   * First chunk MUST be IHDR (13 bytes payload).
//   * Last chunk MUST be IEND (0-byte payload).
//   * Chunk type's case encodes properties: uppercase first letter →
//     critical chunk (decoder must understand); lowercase → ancillary
//     (can be skipped).
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace snapix::smolpng {

// 8-byte PNG signature.  Always the first bytes of a valid PNG.
constexpr uint8_t kSignature[8] = {
    0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
};

// Chunk header is 4 bytes length + 4 bytes type = 8 bytes; CRC trails each
// chunk.  Use these to bound reads.
constexpr size_t kChunkHeaderSize = 8;
constexpr size_t kChunkCrcSize    = 4;

// IHDR payload is exactly 13 bytes.
constexpr size_t kIhdrPayloadSize = 13;

// 4-byte chunk type, packed as a uint32_t (big-endian: first byte → MSB).
// This lets us compare with `chunkType == kIHDR` instead of memcmp.
constexpr uint32_t fourcc(const char a, const char b, const char c,
                          const char d) {
  return (static_cast<uint32_t>(a) << 24) |
         (static_cast<uint32_t>(b) << 16) |
         (static_cast<uint32_t>(c) << 8)  |
         (static_cast<uint32_t>(d));
}

constexpr uint32_t kIHDR = fourcc('I', 'H', 'D', 'R');
constexpr uint32_t kPLTE = fourcc('P', 'L', 'T', 'E');
constexpr uint32_t kIDAT = fourcc('I', 'D', 'A', 'T');
constexpr uint32_t kIEND = fourcc('I', 'E', 'N', 'D');
constexpr uint32_t kTRNS = fourcc('t', 'R', 'N', 'S');
constexpr uint32_t kGAMA = fourcc('g', 'A', 'M', 'A');
constexpr uint32_t kSRGB = fourcc('s', 'R', 'G', 'B');
constexpr uint32_t kPHYS = fourcc('p', 'H', 'Y', 's');
constexpr uint32_t kTEXT = fourcc('t', 'E', 'X', 't');
constexpr uint32_t kZTXT = fourcc('z', 'T', 'X', 't');
constexpr uint32_t kITXT = fourcc('i', 'T', 'X', 't');

// IHDR color types (Table 11.1 of the spec).
enum class ColorType : uint8_t {
  Grayscale     = 0,
  Rgb           = 2,
  Palette       = 3,
  GrayscaleA    = 4,
  RgbA          = 6,
};

// Samples per pixel for each supported (non-palette) color type.
constexpr int samplesPerPixel(const ColorType c) {
  switch (c) {
    case ColorType::Grayscale:  return 1;
    case ColorType::Rgb:        return 3;
    case ColorType::GrayscaleA: return 2;
    case ColorType::RgbA:       return 4;
    case ColorType::Palette:    return 1;  // index, expands via PLTE
  }
  return 0;
}

// PNG filter types (Table 11.4).  Stored as the first byte of every
// filtered scanline.
enum class FilterType : uint8_t {
  None    = 0,
  Sub     = 1,
  Up      = 2,
  Average = 3,
  Paeth   = 4,
};

}  // namespace snapix::smolpng
