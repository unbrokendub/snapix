#pragma once

// =============================================================================
// MarkerizeChapter — drive HtmlStripper over an HTML/FB2 byte stream and
// route the resulting marker bytes to a sink.
//
// PURPOSE (Phase R2 of the v3 architectural refactor):
//   The marker-byte protocol is the data plane for the v3 streaming
//   page-layout pipeline.  Each chapter the EPUB parser already
//   prepares as a single flat HTML file on LittleFS — the same source
//   that feeds Expat in the legacy ChapterHtmlSlimParser path.
//
//   This helper is the side-channel that converts that prepared HTML
//   (or an FB2 chapter body) into the marker stream and writes it to
//   another LittleFS file (a `.markers` sidecar).  The marker stream
//   then feeds StreamingPaginator (Phase R1) for layout, and later
//   GfxRenderer for direct framebuffer drawing (Phase R3).
//
// API SHAPE (callback-based I/O):
//   The helper is platform-agnostic — neither LittleFS nor std::ifstream
//   is referenced here.  Callers wrap their I/O behind two callbacks:
//     `readChunk(buf, bufSize)`  → bytes read, 0 = EOF, <0 = error
//     `writeChunk(data, len)`    → true = success, false = error
//   On ESP32 these wrap LittleFS::File handles.  On host (unit tests)
//   they wrap std::vector<uint8_t> buffers or std::ifstream.
//
// NO HEAP ALLOCATIONS:
//   The chunk buffer is on the caller's stack (passed by ref).  The
//   HtmlStripper internal state is ~600 bytes (member buffers in the
//   class instance).  Total transient: ~1 KB during the markerize pass.
//   Zero heap activity — the whole point of the v3 architecture.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <functional>

#include "HtmlStripper.h"

namespace snapix::smolport {

// Cooperative-abort callback — returning true makes markerizeChapter()
// stop at the next chunk boundary.  Used by the BG worker to interrupt
// the markerize pass when the user presses a button.  Optional — pass
// `{}` (default-constructed std::function) for no abort hook.
using MarkerizeAbortFn = std::function<bool()>;

// Read up to `bufSize` bytes into `buf`.  Returns:
//   * positive value N  → N bytes read into buf[0..N)
//   * zero               → end of input (clean EOF)
//   * negative           → I/O error; markerize aborts
using MarkerizeReadFn = std::function<int(uint8_t* buf, size_t bufSize)>;

// Write exactly `len` bytes from `data`.  Returns:
//   * true   → all bytes written
//   * false  → I/O error; markerize aborts
using MarkerizeWriteFn = std::function<bool(const uint8_t* data, size_t len)>;

// Outcome of a markerize pass.  Aborted is intentional (user input,
// shouldStop callback); IoError is not.
enum class MarkerizeStatus : uint8_t {
  Success,    // EOF reached, all bytes written
  Aborted,    // shouldAbort() returned true mid-pass
  ReadError,  // readChunk() returned negative
  WriteError, // writeChunk() returned false
};

// Stats reported by markerizeChapter for logging / diagnostics.
struct MarkerizeStats {
  uint32_t inputBytes;      // total bytes pulled from readChunk
  uint32_t outputBytes;     // total bytes pushed to writeChunk
  uint16_t chunksProcessed; // how many readChunk calls drained
};

// Markerize a chapter byte stream.
//
// Pulls bytes from `readChunk` in `bufSize`-sized chunks (caller picks
// `bufSize` based on its memory budget; typical ESP32 use: 1024 or 2048).
// Each chunk is fed into `HtmlStripper(mode)`; the stripper's emitted
// marker bytes are forwarded to `writeChunk`.  Loops until readChunk
// returns 0 (EOF), readChunk returns <0 (error), writeChunk returns
// false (error), or shouldAbort() returns true.
//
// On clean completion, the stripper's `finish()` is called — currently a
// no-op, reserved for future trailing markers.
//
// Returns a MarkerizeStatus + populates `outStats` if non-null.
//
// THIS FUNCTION DOES NOT ALLOCATE HEAP.  All I/O buffers are passed in
// by the caller; the stripper's internal state is in-class.  Watchdog-
// safe — call shouldAbort() between chunks.
MarkerizeStatus markerizeChapter(HtmlStripper::Mode mode, MarkerizeReadFn readChunk,
                                  MarkerizeWriteFn writeChunk, uint8_t* chunkBuf, size_t chunkBufSize,
                                  const MarkerizeAbortFn& shouldAbort = {},
                                  MarkerizeStats* outStats = nullptr);

}  // namespace snapix::smolport
