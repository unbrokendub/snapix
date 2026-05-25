#pragma once

// =============================================================================
// SmolJpeg internal state types — Component, HuffTable, JpegState.
//
// Direct ports of smol-epub's `Component`, `HuffTable`, `JpegState`
// (jpeg.rs lines 80-122).  Layout-compatible; size matched as closely
// as cheap.  JpegState is large (~8.4 KB) — v2.0.92 holds a single
// BSS-resident instance inside SmolJpeg.cpp (`sharedJpegState`); each
// decode resets the state via `parseMarkers` before use and serialises
// concurrent callers through a FreeRTOS mutex.  Prior to v2.0.92 the
// decoder allocated a fresh `JpegState` on the heap per call, which
// fragmented chunks of the available DRAM whenever the decoder ran
// concurrently with chapter parse / image-cache build.
// =============================================================================

#include <cstdint>

#include "SmolJpeg.h"

namespace snapix::smoljpeg {

// One JPEG color component (Y, Cb, Cr, or K).  Sampling factors
// describe how many 8×8 blocks per MCU.
struct Component {
  uint8_t id;      // 1=Y, 2=Cb, 3=Cr, 4=K (per JFIF / JPEG SOF)
  uint8_t hSamp;   // horizontal sampling factor (1..4)
  uint8_t vSamp;   // vertical sampling factor (1..4)
  uint8_t qtIdx;   // quantization table index (0..3)
  uint8_t dcTbl;   // DC Huffman table index (0..3)
  uint8_t acTbl;   // AC Huffman table index (0..3)
};

// Huffman decode table.  `lut` is a 256-entry fast lookup for the
// first byte of bitstream input — each entry returns (bit_count,
// symbol).  If the first byte doesn't decode a full code (i.e.
// bit_count > 8) the decoder falls back to bit-serial walk using
// the mincode/maxcode/valptr triplet.
struct HuffTable {
  // First-byte fast LUT: (bitsConsumed, symbol).  bitsConsumed == 0
  // means "no match — fall back to slow path".
  uint8_t lutBits[256];
  uint8_t lutSym[256];

  // Slow-path code search tables, indexed by code length 1..16.
  int32_t mincode[17];
  int32_t maxcode[17];
  int32_t valptr[17];   // smol-epub: usize (we store as int32_t — never > 255)
  uint8_t values[256];
};

// Whole decoder state.  Heap-allocated once per decode, freed before
// return.  Sized to match smol-epub's JpegState ~12 KB.
struct JpegState {
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t numComp = 0;
  Component comp[kMaxComponents] = {};

  // Maximum sampling factors across components — derives MCU dims.
  uint8_t maxH = 1;
  uint8_t maxV = 1;

  // Quantization tables (4 indices × 64 entries).
  uint16_t qt[4][64] = {};
  bool qtOk[4] = {};

  // Huffman tables: 4 DC + 4 AC.
  HuffTable dcHuff[4] = {};
  HuffTable acHuff[4] = {};
  bool dcOk[4] = {};
  bool acOk[4] = {};

  // Restart interval (DRI segment); 0 = no restarts.
  uint16_t restartInterval = 0;

  // Byte offset of the start of entropy-coded data, relative to
  // start of the JPEG stream.  Set by parse_sos.
  uint32_t scanStart = 0;

  // Scan parameters.
  uint8_t scanNumComp = 0;
  uint8_t scanOrder[kMaxComponents] = {};

  bool progressive = false;

  // Progressive-only: spectral selection and successive approximation
  // for the FIRST scan.  Baseline ignores these.
  uint8_t scanSs = 0;   // start of band (0 = DC)
  uint8_t scanSe = 0;   // end of band   (0 = DC only, 63 = all AC)
  uint8_t scanAl = 0;   // approximation low bit (point transform)
};

// =============================================================================
// Zig-zag scan order for inverse-quantization (JPEG spec table T.81 / F.2).
// 64 entries; ZZ[i] gives the natural-order position of the i'th coefficient
// in the zig-zag-decoded block.
// =============================================================================
extern const uint8_t kZigZag[64];

// =============================================================================
// IDCT scaling constants (IJG ISLOW algorithm, CONST_BITS = 13).
// =============================================================================
constexpr int32_t kCB = 13;
constexpr int32_t kP1 = 2;
constexpr int32_t kF0298 = 2446;
constexpr int32_t kF0390 = 3196;
constexpr int32_t kF0541 = 4433;
constexpr int32_t kF0765 = 6270;
constexpr int32_t kF0899 = 7373;
constexpr int32_t kF1175 = 9633;
constexpr int32_t kF1501 = 12299;
constexpr int32_t kF1847 = 15137;
constexpr int32_t kF1961 = 16069;
constexpr int32_t kF2053 = 16819;
constexpr int32_t kF2562 = 20995;
constexpr int32_t kF3072 = 25172;

}  // namespace snapix::smoljpeg
