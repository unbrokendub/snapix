#pragma once

// =============================================================================
// SmolPng — minimal one-pass PNG decoder producing 1-bit Floyd-Steinberg
// dithered bitmaps directly.
//
// Heritage: ported from hansmrtn/smol-epub `png.rs` (MIT/Apache-2.0).
// Pipeline: chunk parse → uzlib deflate → per-scanline filter inversion
// → RGB-to-luma → integer downscale → Floyd-Steinberg dither → 1-bit BMP
// output stream.  Saves the per-image grayscale intermediate buffer that
// the v2.x pngle + 2-bit-Atkinson pipeline maintains (~width × N bytes —
// ~10-20 KB for cover-sized images).
//
// MVP scope (Phase 6 alpha):
//   * Color type 2 (RGB, no alpha) — primary EPUB image format
//   * Color type 6 (RGB+alpha)     — alpha blended against white background
//   * Bit depth 8 only
//   * Filter method 0 (adaptive) only — standard PNG filter set
//   * Compression method 0 (deflate) only — only one defined
//   * Non-interlaced only — Adam7 falls back to pngle
//
// Unsupported (returns UnsupportedFeature):
//   * Color types 0 (grayscale), 3 (palette), 4 (gray+alpha)
//   * Bit depths 1, 2, 4, 16
//   * Adam7 interlacing
//
// Public API: stream-source + stream-sink callbacks.  Caller supplies an
// `InputStream` (random-access via byte-offset reads) and an
// `OutputStream` for the BMP output bytes.  No internal heap state
// between decodes — buffers allocated transiently from the heap during
// decode and freed before return.
//
// Phase 6a status: header + types + chunk parser + IHDR validation only.
// The public `decodeTo1BitBmp` entry point is a stub that returns
// `Status::NotImplemented` until Phase 6b/6c land (uzlib + filter +
// pixel unpack + BMP output).  `peekDimensions` IS implemented — it
// only needs IHDR.
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace snapix::smolpng {

// =============================================================================
// Limits — same as SmolJpeg's profile for consistency on Xteink X4.
// =============================================================================
constexpr uint32_t kMaxPixels       = 2048u * 2048u;
constexpr size_t   kHeaderReadBytes = 4096;  // PNG signature + IHDR + ancillary
                                             // chunks; rarely > 1 KB but cap
                                             // for safety.

// =============================================================================
// Result codes.  Same shape as snapix::smoljpeg::Status for predictable
// integration; bit-exact enum values not required.
// =============================================================================
enum class Status : uint8_t {
  Ok,
  NotImplemented,       // Phase 6a stub
  InputError,           // stream read failed / truncated
  OutputError,          // OutputStream::write failed
  AllocFailed,          // out of memory for transient buffers
  InvalidPng,           // bad signature, CRC mismatch, malformed chunks
  UnsupportedFeature,   // 16-bit, palette, interlaced, etc.
  ExceedsPixelLimit,    // > kMaxPixels — refuse to decode
  Aborted,              // shouldAbort callback returned true mid-decode
};

const char* statusToString(Status s);

// =============================================================================
// InputStream / OutputStream — same shape as the SmolJpeg lib but separate
// types so this library stays self-contained.  Adapters wrap FsFile /
// fs::File on the Arduino side and std::vector on the host side.
// =============================================================================
class InputStream {
 public:
  virtual ~InputStream() = default;
  virtual uint32_t length() const = 0;
  virtual int read(uint32_t offset, uint8_t* buf, size_t len) = 0;
};

class OutputStream {
 public:
  virtual ~OutputStream() = default;
  virtual bool write(const uint8_t* data, size_t len) = 0;
};

// =============================================================================
// Public API.
// =============================================================================

// Decode a PNG into a 1-bit Floyd-Steinberg dithered BMP, writing the
// complete BMP file (header + pixel data) to `bmpOut`.  Output is
// top-down (negative height) so the caller can stream rows in scan
// order without seeking.
//
// Source dimensions are integer-downscaled so the output fits within
// `maxW × maxH` pixels.  Returns Status::NotImplemented in Phase 6a.
Status decodeTo1BitBmp(InputStream& in, OutputStream& bmpOut, int maxW,
                       int maxH, bool (*shouldAbort)() = nullptr);

// Peek source dimensions (parses IHDR only).  Reads ≤32 bytes from the
// front of the stream.  Phase 6a implements this path.
Status peekDimensions(InputStream& in, uint16_t& outWidth,
                      uint16_t& outHeight);

}  // namespace snapix::smolpng
