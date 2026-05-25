#pragma once

// =============================================================================
// SmolJpegHuffman — Huffman-table construction and per-symbol decode.
//
// Tables built from a JPEG DHT segment (bits[16] + values[]).  Decode is
// hybrid:
//   * Fast path: 8-bit prefix LUT covers codes of length ≤ 8.
//   * Slow path: canonical mincode/maxcode/valptr walk for codes 9..16.
//
// Port of smol-epub `build_huff_table` / `decode_huff_symbol`
// (jpeg.rs:357-440).
// =============================================================================

#include "SmolJpegBitReader.h"
#include "SmolJpegState.h"

#include <cstdint>

namespace snapix::smoljpeg {

// Populate `tbl` from a DHT segment's count-of-codes-per-length array
// (`bits`, indexed by length-1) and the symbol value array (`values`).
// `nValues` must equal sum(bits[0..15]) and ≤ 256.
// Returns false on malformed input (codes overflow 16 bits, count mismatch,
// or too many values).
bool buildHuffTable(HuffTable& tbl, const uint8_t bits[16],
                    const uint8_t* values, uint16_t nValues);

// Decode one Huffman symbol from `br` using `tbl`.
// Returns 0..255 on success, or -1 on EOS / invalid code / marker hit
// before a complete code could be read.
int decodeHuffmanSymbol(BitReader& br, const HuffTable& tbl);

}  // namespace snapix::smoljpeg
