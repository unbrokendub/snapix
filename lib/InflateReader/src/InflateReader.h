#pragma once

#include <uzlib.h>

#include <cstddef>

// Return value for readAtMost().
enum class InflateStatus {
  Ok,     // Output buffer full; more compressed data remains.
  Done,   // Stream ended cleanly (TINF_DONE). produced may be < maxLen.
  Error,  // Decompression failed.
};

// Streaming deflate decompressor wrapping uzlib.
//
// Two modes:
//   init(false)  — one-shot: input is a contiguous buffer, call read() once.
//   init(true)   — streaming: allocates a 32KB ring buffer for back-references
//                  across multiple read() / readAtMost() calls.
//   init(true, buf) — streaming with caller-provided 32KB buffer (no malloc).
//
// Streaming callback pattern:
//   The uzlib read callback receives a `struct uzlib_uncomp*` with no separate
//   context pointer. To attach context, make InflateReader the *first member* of
//   your context struct, then cast inside the callback:
//
//     struct MyCtx {
//       InflateReader reader;   // must be first
//       FsFile* file;
//       // ...
//     };
//     static int myCb(struct uzlib_uncomp* u) {
//       MyCtx* ctx = reinterpret_cast<MyCtx*>(u);   // valid: reader.decomp is at offset 0
//       // ... fill u->source / u->source_limit, return first byte
//     }
//     MyCtx ctx;
//     ctx.reader.init(true);
//     ctx.reader.setReadCallback(myCb);
//
class InflateReader {
 public:
  InflateReader() = default;
  ~InflateReader();

  InflateReader(const InflateReader&) = delete;
  InflateReader& operator=(const InflateReader&) = delete;

  // Initialise decompressor. streaming=true allocates a 32KB ring buffer needed
  // when read() or readAtMost() will be called multiple times.
  // Returns false only in streaming mode if the ring buffer allocation fails.
  bool init(bool streaming = false);

  // v2.0.178 — streaming-mode init: heap first, externalBuffer as fallback.
  // (v2.0.177 added a 32 KB BSS scratch as the primary path; reverted in
  // v2.0.178 — see InflateReader.cpp for why.)  The implementation tries
  // malloc(32 KB) first; on success the heap buffer is owned (freed in
  // deinit()).  If malloc fails, it falls back to externalBuffer (must be
  // >= 32 KB, caller-owned lifetime, not freed in deinit()).  Callers
  // typically pass renderer.getFrameBuffer() via Epub's
  // sharedZipDictBuffer() so chapter parses can still proceed when heap
  // is too fragmented to provide 32 KB contiguous — at the cost of a
  // possible display-corruption glitch (see Epub.cpp comments).
  bool init(bool streaming, uint8_t* externalBuffer);

  // v3.11.12 — streaming init with a caller-owned window SMALLER than 32 KB.
  // Valid only when the caller reads at most `windowSize` bytes of output in
  // total: a deflate back-reference can never reach before the start of the
  // stream, so it then always lands inside the window.  Used to sniff image
  // headers with an 8 KB window instead of 32 KB.  Not owned, never claimed.
  bool initWithWindow(uint8_t* window, size_t windowSize);

  // Release the ring buffer (if owned) and reset internal state.
  void deinit();

  // Set the entire compressed input as a contiguous memory buffer.
  // Used in one-shot mode; not needed when a read callback is set.
  void setSource(const uint8_t* src, size_t len);

  // Set a uzlib-compatible read callback for streaming input.
  // See class-level comment for the expected callback/context struct pattern.
  void setReadCallback(int (*cb)(uzlib_uncomp*));

  // Consume the 2-byte zlib header (CMF + FLG) from the input stream.
  // Call this once before the first read() when input is zlib-wrapped (e.g. PNG IDAT).
  void skipZlibHeader();

  // Decompress exactly len bytes into dest.
  // Returns false if the stream ends before producing len bytes, or on error.
  bool read(uint8_t* dest, size_t len);

  // Decompress up to maxLen bytes into dest.
  // Sets *produced to the number of bytes written.
  // Returns Done when the stream ends cleanly, Ok when there is more to read,
  // and Error on failure.
  InflateStatus readAtMost(uint8_t* dest, size_t maxLen, size_t* produced);

  // Returns a pointer to the underlying TINF_DATA.
  uzlib_uncomp* raw() { return &decomp; }

  // ---------------------------------------------------------------------------
  // v3.11.12 — external-dictionary arbitration.  The external buffer is, in
  // practice, the display FRAMEBUFFER (Epub's sharedZipDictBuffer()), used as
  // uzlib's 32 KB window when the heap is too fragmented.  A background inflate
  // and a UI frame must never use it at the same time:
  //   * UI drawing into a live dictionary corrupts every later back-reference
  //     → mojibake / garbage bytes in the chapter text that the markerizer
  //     caches permanently (the v2.0.155 "хпÿÿите" symptom);
  //   * inflate writing into a frame being drawn / uploaded → garbage on the
  //     panel (the "noise behind the Loading… banner" glitch).
  // init() claims the buffer before using it (waits ≤ 2 s for a UI frame to
  // finish, else fails like a failed malloc); deinit() releases it and marks
  // the framebuffer as clobbered.  The UI brackets each frame with
  // beginDisplayFrame()/endDisplayFrame().
  // ---------------------------------------------------------------------------
  // 0 = an inflate holds the framebuffer (do not draw), 1 = acquired,
  // 2 = already held by this task (nested frame — do not release).
  static uint8_t beginDisplayFrame();
  static void endDisplayFrame(uint8_t token);
  // True while a background inflate is using the framebuffer as its window.
  static bool externalDictInUse();
  // True once after an inflate used the framebuffer: its pixels are garbage,
  // so a frame that draws OVER existing content must clear the screen first.
  static bool consumeExternalDictClobbered();

 private:
  uzlib_uncomp decomp = {};
  uint8_t* ringBuffer = nullptr;
  bool ownsRingBuffer = false;
  uint8_t externalClaim = 0;  // v3.11.12 — 0 none, 1 exclusive, 2 nested in own UI frame
  // v2.0.178 — removed v2.0.177's `usingBssScratch` flag and the
  // associated BSS scratch buffer; reverted to v2.0.176's heap-first/
  // externalBuffer-fallback strategy.  See InflateReader.cpp top comment.
};
