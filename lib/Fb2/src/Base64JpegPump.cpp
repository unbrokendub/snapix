#include "Base64JpegPump.h"

#include <SDCardManager.h>
#include <SharedSpiLock.h>

#include <algorithm>
#include <cstring>

namespace {

// Same lookup as Fb2.cpp's base64DecodeChar — duplicated so this file stays
// self-contained.  Returns -1 for non-base64 / whitespace, -2 for `=` padding,
// 0..63 for valid base64 alphabet chars.
inline int8_t base64DecodeChar(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return static_cast<int8_t>(c - 'A');
  if (c >= 'a' && c <= 'z') return static_cast<int8_t>(c - 'a' + 26);
  if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0' + 52);
  if (c == '+') return 62;
  if (c == '/') return 63;
  if (c == '=') return -2;
  return -1;
}

}  // namespace

void Base64JpegPump::init(FsFile& src, uint32_t offset, uint32_t length) {
  src_ = &src;
  srcStart_ = offset;
  srcEnd_ = offset + length;
  srcReadPos_ = offset;
  logicalSize_ = (length * 3) / 4;  // base64 → bytes ratio
  logicalPos_ = 0;
  accum_ = 0;
  accumBits_ = 0;
  foundOpenTagEnd_ = false;
  srcBufPos_ = 0;
  srcBufFilled_ = 0;

  // Position the source file at the slice start.  Subsequent reads happen
  // sequentially from here.
  if (src_) {
    snapix::spi::SharedBusLock lk;
    src_->seek(srcStart_);
  }
}

void Base64JpegPump::rewind() {
  // JPEGDEC requested a back-seek.  Restart from slice start and let the
  // caller (pfnSeek) fast-forward to the requested logical position.
  if (src_) {
    snapix::spi::SharedBusLock lk;
    src_->seek(srcStart_);
  }
  srcReadPos_ = srcStart_;
  logicalPos_ = 0;
  accum_ = 0;
  accumBits_ = 0;
  foundOpenTagEnd_ = false;
  srcBufPos_ = 0;
  srcBufFilled_ = 0;
}

bool Base64JpegPump::refillSourceBuffer() {
  if (!src_) return false;
  if (srcReadPos_ >= srcEnd_) return false;

  const uint32_t remaining = srcEnd_ - srcReadPos_;
  const uint32_t toRead = std::min<uint32_t>(static_cast<uint32_t>(kSrcBufSize), remaining);

  int got;
  {
    snapix::spi::SharedBusLock lk;
    got = src_->read(srcBuf_, toRead);
  }
  if (got <= 0) return false;

  srcBufPos_ = 0;
  srcBufFilled_ = static_cast<size_t>(got);
  srcReadPos_ += static_cast<uint32_t>(got);
  return true;
}

size_t Base64JpegPump::produceBytes(uint8_t* outBuf, size_t want) {
  size_t produced = 0;
  while (produced < want) {
    if (srcBufPos_ >= srcBufFilled_) {
      if (!refillSourceBuffer()) break;
    }

    const uint8_t c = srcBuf_[srcBufPos_++];
    if (!foundOpenTagEnd_) {
      // The `<binary id="...">` opening tag prefix lives inside our slice
      // (since BinaryEntry::fileOffset points at the tag start, not at the
      // base64 payload).  Skip past it by waiting for the closing '>'.
      if (c == '>') foundOpenTagEnd_ = true;
      continue;
    }
    const int8_t v = base64DecodeChar(c);
    if (v < 0) continue;  // whitespace, '=' padding, anything non-base64

    accum_ = (accum_ << 6) | static_cast<uint32_t>(static_cast<uint8_t>(v));
    accumBits_ += 6;
    if (accumBits_ >= 8) {
      accumBits_ -= 8;
      outBuf[produced++] = static_cast<uint8_t>((accum_ >> accumBits_) & 0xFFu);
    }
  }
  logicalPos_ += static_cast<uint32_t>(produced);
  return produced;
}

int32_t Base64JpegPump::pfnRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  if (!pFile || iLen <= 0) return 0;
  auto* pump = static_cast<Base64JpegPump*>(pFile->fHandle);
  if (!pump) return 0;
  return static_cast<int32_t>(pump->produceBytes(pBuf, static_cast<size_t>(iLen)));
}

int32_t Base64JpegPump::pfnSeek(JPEGFILE* pFile, int32_t iPosition) {
  if (!pFile || iPosition < 0) return 0;
  auto* pump = static_cast<Base64JpegPump*>(pFile->fHandle);
  if (!pump) return 0;

  if (static_cast<uint32_t>(iPosition) == pump->logicalPos_) return 1;  // no-op

  if (static_cast<uint32_t>(iPosition) < pump->logicalPos_) {
    // Back-seek: rewind everything and fast-forward.  Baseline JPEG decode
    // shouldn't trigger this in practice — the seek callback fires mostly
    // for forward-only segment skipping during the SOF/DHT/DQT preamble.
    pump->rewind();
  }

  // Forward-skip: discard bytes until we're at the requested position.  Use
  // a small stack scratch buffer to avoid heap pressure.
  uint8_t scratch[256];
  uint32_t toSkip = static_cast<uint32_t>(iPosition) - pump->logicalPos_;
  while (toSkip > 0) {
    const size_t chunk = std::min<size_t>(sizeof(scratch), toSkip);
    const size_t got = pump->produceBytes(scratch, chunk);
    if (got == 0) return 0;  // ran out of source before reaching iPosition
    toSkip -= static_cast<uint32_t>(got);
  }
  return 1;
}

void Base64JpegPump::pfnClose(void* /*pHandle*/) {
  // FsFile lifetime is owned by the caller of init() — we don't close it.
}

// ============================================================================
// JPEG SOF parser — extracts width × height from the first
// SOF0/SOF1/SOF2 marker in a JPEG byte stream.
//
// JPEG marker structure (standalone markers excluded — they have no payload):
//   FF Mn LL LL <payload>           where LL LL is the BIG-ENDIAN length
//                                   field INCLUDING its own 2 bytes.
//
// SOF0/1/2 payload layout (first 6 bytes after length):
//   precision (1 byte) | height (2 bytes BE) | width (2 bytes BE) | comps (1)
//
// Standalone markers (no length, no payload):
//   FF 01 (TEM)
//   FF D0..D7 (RST0..RST7)
//   FF D8 (SOI)
//   FF D9 (EOI)
//
// Skip everything that isn't an SOF marker until we hit one or run out of
// buffer.  Typical SOF is within the first ~200-500 bytes of a JPEG.
// ============================================================================
bool parseJpegSofDims(const uint8_t* data, size_t len, uint16_t* outW, uint16_t* outH) {
  if (!data || !outW || !outH || len < 4) return false;
  if (data[0] != 0xFF || data[1] != 0xD8) return false;  // SOI

  size_t i = 2;
  while (i + 4 <= len) {
    // Marker prefix is 0xFF.  Skip 0xFF padding (some encoders emit runs).
    if (data[i] != 0xFF) {
      ++i;
      continue;
    }
    while (i + 1 < len && data[i + 1] == 0xFF) ++i;
    if (i + 1 >= len) return false;

    const uint8_t marker = data[i + 1];

    // SOF0 (baseline), SOF1 (extended), SOF2 (progressive).
    if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
      // Need: i+2..i+3 = length, i+4 = precision, i+5..6 = height, i+7..8 = width.
      if (i + 9 > len) return false;
      *outH = static_cast<uint16_t>((data[i + 5] << 8) | data[i + 6]);
      *outW = static_cast<uint16_t>((data[i + 7] << 8) | data[i + 8]);
      return *outW > 0 && *outH > 0;
    }

    // Standalone markers: TEM + RST0..RST7 + SOI + EOI.
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
      i += 2;
      continue;
    }

    // All remaining markers carry a 2-byte big-endian length INCLUDING the
    // length field itself.
    if (i + 4 > len) return false;
    const uint16_t segLen = static_cast<uint16_t>((data[i + 2] << 8) | data[i + 3]);
    if (segLen < 2) return false;
    i += 2 + segLen;
  }
  return false;
}
