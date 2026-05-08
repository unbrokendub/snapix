#pragma once

#include <JPEGDEC.h>
#include <SdFat.h>

#include <cstddef>
#include <cstdint>

// ============================================================================
// Base64JpegPump
//
// Stateful base64-decoder-as-JPEGDEC-pump.  Reads base64 chars on-demand from
// a SLICE of an FsFile (the FB2 source's `<binary>` block), decodes them into
// JPEG bytes inline, and serves the bytes through JPEGDEC's pfnRead/pfnSeek
// callbacks WITHOUT ever materialising the full JPEG to disk.
//
// Why this exists: the v2.0.x pipeline pre-staged the entire ~150 KB JPEG to
// `pending/<id>.jpg` before decoding, paying ~600 ms (post-buffer-bump) of
// SD-write time just to write the JPEG, then ~500 ms more reading it back.
// JPEGDEC naturally streams forward through baseline JPEG so we can feed it
// bytes inline as we decode the base64 — saving the round-trip entirely.
//
// Heap footprint: 4 KB source-read buffer (one SD cluster) + ~32 bytes of
// state.  Caller owns the FsFile.
//
// Forward-only seek: JPEGDEC seeks ~rarely during baseline decode (header
// parse + a few segment skips).  All seeks observed are forward; we
// implement back-seek by restarting from `srcStart_` and fast-forwarding.
// ============================================================================
class Base64JpegPump {
 public:
  Base64JpegPump() = default;

  // Bind the pump to a slice of an FsFile.  `offset` must point at (or near)
  // the start of a `<binary id="...">` opening tag — the pump auto-detects
  // the closing `>` and skips everything before it, so the caller can pass
  // either the tag start (BinaryEntry::fileOffset) or the base64-data start.
  // `length` is the byte count of the slice (BinaryEntry::byteLength).
  void init(FsFile& src, uint32_t offset, uint32_t length);

  // Reset to the beginning of the slice.  JPEGDEC's seek callback uses this
  // when it requests a position before our current logical position.
  void rewind();

  // Direct byte-producer entrypoint (used by Fb2::peekImageDims to pull just
  // the first ~1 KB of decoded JPEG bytes for SOF parsing, no JPEGDEC alloc).
  // Returns the number of bytes actually written (may be less than `want` at
  // end-of-stream).
  size_t produceBytes(uint8_t* outBuf, size_t want);

  // Estimated JPEG byte count (= base64 byte length × 3 / 4).  Used as the
  // `iSize` argument to JPEGDEC::open().  The actual decoded length may be
  // a few bytes off because we drop whitespace/padding chars; JPEGDEC only
  // uses iSize as an upper bound for seek validation, so a small overshoot
  // is harmless.
  uint32_t logicalSize() const { return logicalSize_; }

  // Number of decoded JPEG bytes already produced — public so the seek
  // callback can compute forward-skip distance without poking internals.
  uint32_t logicalPos() const { return logicalPos_; }

  // ----------------------------------------------------------------------
  // JPEGDEC callback adapters.  Pass `&Base64JpegPump::pfnRead` etc to
  // JPEGDEC::open(), with the JPEGFILE.fHandle set to the pump instance.
  // The static signature matches the JPEG_*_CALLBACK typedefs in JPEGDEC.h.
  // ----------------------------------------------------------------------
  static int32_t pfnRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t iLen);
  static int32_t pfnSeek(JPEGFILE* pFile, int32_t iPosition);
  static void pfnClose(void* pHandle);

 private:
  bool refillSourceBuffer();

  FsFile* src_ = nullptr;
  uint32_t srcStart_ = 0;       // FB2 byte offset where the slice begins
  uint32_t srcEnd_ = 0;          // FB2 byte offset where the slice ends (exclusive)
  uint32_t srcReadPos_ = 0;      // next byte to read from FB2 source

  uint32_t logicalSize_ = 0;     // estimated decoded JPEG size
  uint32_t logicalPos_ = 0;      // bytes of JPEG already produced

  // Base64 decode state — same accumulator pattern as base64DecodeChar
  // in Fb2.cpp.  6 bits per accepted char, byte boundary every 4 chars.
  uint32_t accum_ = 0;
  int accumBits_ = 0;
  bool foundOpenTagEnd_ = false;  // skipped past the '<binary ...>' opening

  // Source-side buffer (one SD cluster).  Holds raw FB2 bytes pre-decode.
  static constexpr size_t kSrcBufSize = 4096;
  uint8_t srcBuf_[kSrcBufSize] = {};
  size_t srcBufPos_ = 0;
  size_t srcBufFilled_ = 0;
};

// Standalone helper: parse a JPEG byte stream and extract Width/Height from
// the first SOF0/SOF1/SOF2 marker.  Returns true on success.  Used by
// Fb2::peekImageDims to grab dimensions from the first ~1 KB of bytes
// emitted by Base64JpegPump — much cheaper than allocating a full JPEGDEC
// just for header parsing.
bool parseJpegSofDims(const uint8_t* data, size_t len, uint16_t* outW, uint16_t* outH);
