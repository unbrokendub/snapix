#include "SmolPngChunk.h"

#include <cstring>

namespace snapix::smolpng {

namespace {

inline uint32_t getBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8)  |
          static_cast<uint32_t>(p[3]);
}

bool readExact(InputStream& in, uint32_t offset, uint8_t* buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    const int n = in.read(offset + static_cast<uint32_t>(got),
                          buf + got, len - got);
    if (n <= 0) return false;
    got += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

bool readSignature(InputStream& in) {
  uint8_t buf[sizeof(kSignature)];
  if (!readExact(in, 0, buf, sizeof(buf))) return false;
  return std::memcmp(buf, kSignature, sizeof(kSignature)) == 0;
}

bool readChunkHeader(InputStream& in, const uint32_t offset, ChunkHeader& out) {
  uint8_t buf[kChunkHeaderSize];
  if (!readExact(in, offset, buf, sizeof(buf))) return false;
  out.length        = getBE32(buf);
  out.type          = getBE32(buf + 4);
  out.payloadOffset = offset + static_cast<uint32_t>(kChunkHeaderSize);
  return true;
}

Status parseIhdr(InputStream& in, const uint32_t payloadOffset,
                  PngState& state) {
  uint8_t buf[kIhdrPayloadSize];
  if (!readExact(in, payloadOffset, buf, sizeof(buf))) {
    return Status::InputError;
  }

  state.width        = getBE32(buf);
  state.height       = getBE32(buf + 4);
  state.bitDepth     = buf[8];
  state.colorType    = static_cast<ColorType>(buf[9]);
  state.compression  = buf[10];
  state.filterMethod = buf[11];
  state.interlace    = buf[12];

  if (state.width == 0 || state.height == 0) return Status::InvalidPng;
  if (static_cast<uint64_t>(state.width) * state.height > kMaxPixels) {
    return Status::ExceedsPixelLimit;
  }
  if (state.bitDepth != 8) return Status::UnsupportedFeature;
  if (state.colorType != ColorType::Rgb && state.colorType != ColorType::RgbA) {
    return Status::UnsupportedFeature;
  }
  if (state.compression  != 0) return Status::UnsupportedFeature;
  if (state.filterMethod != 0) return Status::UnsupportedFeature;
  if (state.interlace    != 0) return Status::UnsupportedFeature;

  state.bytesPerPixel = static_cast<uint8_t>(samplesPerPixel(state.colorType));
  state.rowStride     = state.width * state.bytesPerPixel;
  return Status::Ok;
}

Status scanThroughIdat(InputStream& in, PngState& state) {
  if (!readSignature(in)) return Status::InvalidPng;

  uint32_t offset = static_cast<uint32_t>(sizeof(kSignature));
  bool sawIhdr = false;

  for (;;) {
    if (offset >= in.length()) return Status::InvalidPng;  // truncated

    ChunkHeader hdr;
    if (!readChunkHeader(in, offset, hdr)) return Status::InvalidPng;

    if (!sawIhdr) {
      if (hdr.type != kIHDR) return Status::InvalidPng;
      if (hdr.length != kIhdrPayloadSize) return Status::InvalidPng;
      const Status s = parseIhdr(in, hdr.payloadOffset, state);
      if (s != Status::Ok) return s;
      sawIhdr = true;
      offset = hdr.payloadOffset + hdr.length + static_cast<uint32_t>(kChunkCrcSize);
      continue;
    }

    if (hdr.type == kIDAT) {
      state.firstIdatOffset = offset;
      return Status::Ok;
    }

    if (hdr.type == kIEND) {
      // EOI before any IDAT — malformed.
      return Status::InvalidPng;
    }

    // Some critical chunks we can't ignore.  PLTE for palette images
    // would require us to load the palette, but we reject palette
    // color type up-front in parseIhdr, so PLTE here is benign for our
    // input set — skip it like any ancillary chunk.
    offset = hdr.payloadOffset + hdr.length + static_cast<uint32_t>(kChunkCrcSize);
  }
}

}  // namespace snapix::smolpng
