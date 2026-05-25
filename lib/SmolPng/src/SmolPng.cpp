#include "SmolPng.h"

#include "SmolPngChunk.h"
#include "SmolPngDecode.h"
#include "SmolPngState.h"

namespace snapix::smolpng {

const char* statusToString(const Status s) {
  switch (s) {
    case Status::Ok:                 return "ok";
    case Status::NotImplemented:     return "not-implemented";
    case Status::InputError:         return "input-error";
    case Status::OutputError:        return "output-error";
    case Status::AllocFailed:        return "alloc-failed";
    case Status::InvalidPng:         return "invalid-png";
    case Status::UnsupportedFeature: return "unsupported-feature";
    case Status::ExceedsPixelLimit:  return "exceeds-pixel-limit";
    case Status::Aborted:            return "aborted";
  }
  return "?";
}

// =============================================================================
// Phase 6a public API.  Full `decodeTo1BitBmp` waits on Phase 6b (uzlib
// integration) and 6c (scanline filter + dither pipeline).
// =============================================================================

Status decodeTo1BitBmp(InputStream& in, OutputStream& bmpOut, const int maxW,
                       const int maxH, bool (*shouldAbort)()) {
  PngState state;
  const Status parseRc = scanThroughIdat(in, state);
  if (parseRc != Status::Ok) return parseRc;
  return decodeBaselineStream(in, state, maxW, maxH, bmpOut, shouldAbort);
}

Status peekDimensions(InputStream& in, uint16_t& outWidth,
                      uint16_t& outHeight) {
  outWidth = 0;
  outHeight = 0;

  if (!readSignature(in)) return Status::InvalidPng;

  ChunkHeader hdr;
  if (!readChunkHeader(in, static_cast<uint32_t>(sizeof(kSignature)), hdr)) {
    return Status::InputError;
  }
  if (hdr.type != kIHDR || hdr.length != kIhdrPayloadSize) {
    return Status::InvalidPng;
  }

  PngState st;
  const Status s = parseIhdr(in, hdr.payloadOffset, st);
  if (s != Status::Ok) return s;

  // The on-wire IHDR encodes width/height as u32, but the public API
  // returns u16 — clamp at 0xFFFF, leave the caller's pixel-limit check
  // to flag genuinely huge images.
  outWidth  = static_cast<uint16_t>(st.width  > 0xFFFFu ? 0xFFFFu : st.width);
  outHeight = static_cast<uint16_t>(st.height > 0xFFFFu ? 0xFFFFu : st.height);
  return Status::Ok;
}

}  // namespace snapix::smolpng
