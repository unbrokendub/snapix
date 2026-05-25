#pragma once

// =============================================================================
// StreamingChapterRenderer — orchestrator that drives a marker file from
// disk through MarkerStreamReader → MarkerObserver chain.
//
// PURPOSE (Phase R3 of the v3 architectural refactor):
//   Markerize side-channel from R2 produces `.markers` files on LittleFS.
//   StreamingPaginator from R1 consumes MarkerObserver events and emits
//   per-line layout decisions.  This file is the glue that takes the
//   data plane (markers file on disk) and pushes it through the layout
//   plane (MarkerStreamReader → observer) without ever materialising
//   the marker bytes into a single in-memory buffer.
//
// API SHAPE (callback-based source):
//   The renderer is platform-agnostic — neither LittleFS nor std::ifstream
//   is referenced here.  Caller wraps source I/O behind a `ReadFn`
//   callback (same shape as the markerize helper).  On ESP32 the caller
//   wraps a `LittleFS::File`; on host (unit tests) it wraps an in-memory
//   buffer or `std::ifstream`.
//
// LIFECYCLE:
//   1. Construct with a `MarkerObserver&` (typically `StreamingPaginator`,
//      but unit tests can pass a recording observer).
//   2. Call `renderRange(readFn, chunkBuf, chunkBufSize, abort)` to walk
//      the marker file from current position to EOF or until the observer
//      returns Stop.
//   3. Inspect return code: Success / EofClean / ObserverStopped /
//      ReadError.
//
// WHY A SEPARATE CLASS (not a free function):
//   * State across calls — caller can resume from a partial render after
//     observer returned Stop (e.g. paginator hit page-full, caller
//     advances to next page and continues).
//   * Diagnostics — counts bytes consumed / events dispatched, useful
//     for "why did this page take so long" debugging.
//
// NO HEAP ALLOCATIONS:
//   The chunk buffer is on the caller's stack (passed by ref).  The
//   MarkerStreamReader internal state is ~600 bytes (member of class
//   instance).  Total transient: ~1 KB during a render pass.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <functional>

#include "MarkerStream.h"

namespace snapix::smolport {

// Read up to `bufSize` bytes from the marker source.  Same shape as
// MarkerizeReadFn — positive=bytes, 0=EOF, negative=error.
using ChapterReadFn = std::function<int(uint8_t* buf, size_t bufSize)>;

// Cooperative-abort callback.  Optional — pass `{}` for no abort hook.
using ChapterAbortFn = std::function<bool()>;

// Outcome of a render pass.
enum class RenderStatus : uint8_t {
  EofClean,         // reached end of marker stream cleanly
  ObserverStopped,  // observer returned Stop mid-stream (caller resumes)
  Aborted,          // shouldAbort() returned true
  ReadError,        // readChunk() returned negative
  ReaderError,      // MarkerStreamReader rejected the byte stream
};

// Stats reported by renderRange for diagnostics.
struct RenderStats {
  uint32_t bytesConsumed;     // total bytes pulled from readChunk
  uint16_t chunksProcessed;   // how many readChunk calls drained
  uint8_t  flags;             // bit 0 = stream EOF reached during call
};

class StreamingChapterRenderer {
 public:
  explicit StreamingChapterRenderer(MarkerObserver& observer);

  // Walk the marker file from caller's current source position to EOF.
  // The reader's internal state survives across renderRange() calls so
  // resumption is automatic — caller just keeps the same file handle
  // open and re-invokes after handling the previous Stop.
  //
  // `chunkBuf` is caller-supplied scratch (typical 1024 bytes; lives on
  // caller's stack or BSS).
  RenderStatus renderRange(ChapterReadFn readChunk, uint8_t* chunkBuf, size_t chunkBufSize,
                            const ChapterAbortFn& shouldAbort = {}, RenderStats* outStats = nullptr);

  // Reset the internal MarkerStreamReader state.  Call when starting a
  // fresh chapter (vs resuming after observer-Stop on the same chapter).
  void resetForNextChapter();

  // Forward the underlying reader's `finish()` — fires `onStreamEnd` on
  // the observer.  Call after the final renderRange that returned EofClean.
  bool finish();

 private:
  MarkerStreamReader reader_;
};

}  // namespace snapix::smolport
