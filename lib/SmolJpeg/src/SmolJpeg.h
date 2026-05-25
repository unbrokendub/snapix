#pragma once

// =============================================================================
// SmolJpeg — minimal baseline JPEG decoder producing 1-bit Floyd-Steinberg
// dithered bitmaps directly.
//
// Heritage: ported from hansmrtn/smol-epub `jpeg.rs` (MIT/Apache-2.0).
// One-pass design: chrominance is Huffman-decoded to advance the bitstream
// then discarded; only the Y (luminance) channel is IDCT'd into MCU-row
// buffers, then dithered straight into the 1-bit output bitmap.  Saves
// the per-image grayscale intermediate buffer that the v2.x JPEGDEC +
// separate-dither pipeline maintains (~width×MCU_height bytes — ~20-30 KB
// for a 600×800 source).
//
// Public API: stream-source + stream-sink callbacks.  Caller supplies an
// `InputStream` (random-access via byte-offset reads) and a `Print&` for
// the BMP output bytes.  No internal heap state between decodes — buffers
// allocated transiently from the heap during decode and freed before
// return.
//
// Phase 2a status: header + types + helpers + IDCT + dither only.  The
// public `decodeTo1BitBmp` entry point is a stub that always returns
// `Result::NotImplemented` until Phases 2b/2c/2d land.
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace snapix::smoljpeg {

// =============================================================================
// Limits — same as smol-epub.  Two 8 KB DCT tables, 4 components, 2048×2048
// max pixels (safety cap; real EPUB JPEGs are typically <1600×1600).
// =============================================================================
constexpr size_t kMaxComponents = 4;
constexpr uint32_t kMaxPixels = 2048u * 2048u;

// Streaming read chunk size during MCU decode.  Matches smol-epub.
//
// v2.0.88: the legacy `kHeaderReadBytes` (32 KB) constant was removed
// when the parser was rewritten to stream from `InputStream` directly
// with a 256-byte sliding window — see SmolJpegParse.cpp's
// `HeaderReader`.  Marker parsing no longer pre-allocates a header
// scratch buffer.
constexpr size_t kChunkSize = 4096;

// =============================================================================
// JPEG marker bytes (segment ID following 0xFF).
// =============================================================================
constexpr uint8_t kMarkerSOF0 = 0xC0;  // Start Of Frame — baseline DCT
constexpr uint8_t kMarkerSOF2 = 0xC2;  // Start Of Frame — progressive DCT
constexpr uint8_t kMarkerDHT  = 0xC4;  // Define Huffman Table
constexpr uint8_t kMarkerSOI  = 0xD8;  // Start Of Image
constexpr uint8_t kMarkerEOI  = 0xD9;  // End Of Image
constexpr uint8_t kMarkerSOS  = 0xDA;  // Start Of Scan
constexpr uint8_t kMarkerDQT  = 0xDB;  // Define Quantization Table
constexpr uint8_t kMarkerDRI  = 0xDD;  // Define Restart Interval
constexpr uint8_t kMarkerRST0 = 0xD0;  // Restart marker base (RST0..RST7)
constexpr uint8_t kMarkerRST7 = 0xD7;

// =============================================================================
// Result codes.  Same semantics as smol-epub `Result<T, &'static str>` —
// success or a static error string.
// =============================================================================
enum class Status : uint8_t {
  Ok,
  NotImplemented,        // Phase 2a stub
  InputError,            // stream read failed / truncated
  OutputError,           // Print& write failed
  AllocFailed,           // out of memory for transient buffers
  InvalidJpeg,           // bad markers, missing tables, malformed bitstream
  UnsupportedFeature,    // component count > 4, sampling > 4, etc.
  ProgressiveScanLimit,  // progressive JPEG: first scan only (partial decode)
  ExceedsPixelLimit,     // > kMaxPixels — refuse to decode
  Aborted,               // shouldAbort callback returned true mid-decode
};

const char* statusToString(Status s);

// =============================================================================
// InputStream — random-access read abstraction for the JPEG source.
//
// `read(offset, buf, len)` reads up to `len` bytes at absolute offset.
// Returns bytes actually read (may be short at EOF).  Returns -1 on
// hard I/O error.  smol-epub uses an FnMut closure; in C++ we use a
// virtual interface so caller can wrap fs::File / FsFile / memory buf
// without templates.
// =============================================================================
class InputStream {
 public:
  virtual ~InputStream() = default;

  // Total stream length in bytes.
  virtual uint32_t length() const = 0;

  // Read at absolute offset.  Returns number of bytes copied (0..len).
  // Returns -1 on hard error.
  virtual int read(uint32_t offset, uint8_t* buf, size_t len) = 0;
};

// =============================================================================
// OutputStream — byte-sink for the 1-bit BMP output.
//
// `write` returns true if ALL `len` bytes were accepted (partial-write
// is treated as failure to keep the decoder simple).  On Arduino, wrap
// a `Print&` in a tiny adapter; for host tests, capture into a buffer.
// =============================================================================
class OutputStream {
 public:
  virtual ~OutputStream() = default;

  // Write `len` bytes.  Returns true on full success, false on error.
  virtual bool write(const uint8_t* data, size_t len) = 0;
};

// =============================================================================
// Public API.
// =============================================================================

// Decode a JPEG into a 1-bit Floyd-Steinberg dithered BMP, writing the
// complete BMP file (header + pixel data) to `bmpOut`.  Output is
// top-down (negative height in the BITMAPINFOHEADER) so the caller can
// write rows in scan order without seeking.
//
// Source dimensions are integer-downscaled so the output fits within
// `maxW × maxH` pixels.  When source is already smaller, `scale = 1`
// and the output matches source dims.
//
// `shouldAbort` (optional) is polled every MCU row; if it returns true
// the decode bails with `Aborted`.  Pass nullptr to disable.
Status decodeTo1BitBmp(InputStream& in, OutputStream& bmpOut, int maxW,
                       int maxH, bool (*shouldAbort)() = nullptr);

// Peek source dimensions (parses SOF0/SOF2 only, no decode).  Used by
// the page-layout pass to reserve image height before deciding to
// actually decode pixels.  Cheap: typically <1 KB read.
Status peekDimensions(InputStream& in, uint16_t& outWidth, uint16_t& outHeight);

}  // namespace snapix::smoljpeg
