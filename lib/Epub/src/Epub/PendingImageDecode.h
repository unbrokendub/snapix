#pragma once

// v2.0.83: deferred image decode queue.
//
// During chapter indexing the chapter parser extracts each inline image from
// the EPUB ZIP into a LittleFS temp file, then synchronously converts it to a
// dithered BMP.  The convert step costs ~3-5 s per 240×300 image on ESP32-C3
// — peak ~10 s for big sources.  Over a heavy chapter (Calibre-export EPUB
// with 5+ images per spine) that's 30-60 s of CPU spent blocking the layout
// pipeline.  The user-visible symptom: TOC jump to a chapter with images
// takes minutes; each hot-extend batch stalls on the next image even though
// page layout itself only needs DIMENSIONS, not pixel data.
//
// This queue decouples the two phases.  The parser extracts the temp JPEG
// (cheap-ish: ~2-3 s — ZIP inflate + LittleFS write) AND peeks dimensions
// from the JFIF SOF marker (~50 ms).  It then enqueues a record describing
// the deferred BMP convert and returns to the layout pipeline immediately.
// The page is committed to the section cache with an ImageBlock pointing at
// the future BMP path; render falls back to a placeholder until the BMP
// materialises.
//
// A worker (drainOne / drainAll) pulled by the BG cache task drains the
// queue between page batches.  Each completed decode signals
// `consumeRefreshSignal()` so the reader knows to re-render — pages whose
// ImageBlocks newly resolved will pick up the real bitmap on the next
// render pass.
//
// Thread safety: the queue is guarded by a FreeRTOS mutex.  Enqueue is
// called from the parser running on the BG cache task; drain is called from
// the same task during its idle/post-batch passes.  Refresh-signal poll is
// called from the UI task.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace snapix {

struct PendingImageDecode {
  std::string tempJpegPath;    // /cache/epub_<hash>/images/.tmp_<srcHash>.jpg
  std::string targetBmpPath;   // /cache/epub_<hash>/images/<srcHash>.bmp
  // Optional lazy source preparation.  Foreground streaming render enqueues
  // this callback instead of inflating the EPUB entry synchronously.  The
  // ReaderAsync worker invokes it immediately before conversion.
  std::function<bool(const std::string&, const std::function<bool()>&)>
      prepareInput;
  uint16_t maxWidth;
  uint16_t maxHeight;
  uint32_t srcHash;            // session-blacklist key
  bool quickMode;
  // v2.0.88: when true, drainOne routes through the SmolJpeg one-pass
  // decoder (oneBit=true in the ImageConvertConfig).  Defaults to true
  // because the reader runs in 1-bit mode anyway — inline EPUB images
  // are dithered to B/W on render either way.  Faster decode + smaller
  // peak heap than the legacy picojpeg + 2-bit Atkinson pipeline.
  bool oneBit = true;
  const char* logTag;
};

namespace pendingImage {

enum class Priority : uint8_t {
  Background = 0,
  CurrentPage = 1,
};

// Push a new pending decode.  Returns false if the queue is full (current
// cap is 32 entries — enough for any single chapter we've seen, and small
// enough that worst-case temp-file disk usage stays under 2 MB on LittleFS).
// Duplicate target paths are coalesced; a CurrentPage duplicate is promoted.
bool enqueue(PendingImageDecode item, Priority priority = Priority::Background);

// Move an already-queued target to the front.  drainTarget atomically removes
// and drains that exact item (or reports true when another task is already
// decoding it); it never accidentally drains a different image.
bool promote(const std::string& targetBmpPath);
bool drainTarget(
    const std::string& targetBmpPath,
    const std::function<bool()>& shouldAbort = {});
bool isPendingOrActive(const std::string& targetBmpPath);

// Pop one pending decode off the queue, run the JPEG→BMP convert, delete
// the temp JPEG on success, blacklist on hard failure.  Returns true if it
// did work, false if the queue was empty.  Designed to be called from the
// BG cache task in a loop until it returns false or shouldAbort fires.
bool drainOne(const std::function<bool()>& shouldAbort = {});

// Drain everything currently queued.  Caller passes a `shouldAbort` to
// allow cooperative cancellation between decodes (each decode is 3-10 s and
// can't be interrupted internally; we yield between items, not within).
size_t drainAll(const std::function<bool()>& shouldAbort = {});

// Inspect.
size_t pendingCount();
bool empty();

// Drop everything matching the given target-BMP-path prefix.  Called on
// book close / cache clear so stale temp files for the closed book don't
// linger.  Returns number of entries removed; temp files are unlinked.
size_t purgePrefix(const std::string& bmpPathPrefix);

// "BMP appeared since you last asked" — UI poll.  consumeRefreshSignal()
// reads the flag atomically and clears it.  When true, the UI should
// invalidate page caches that contain ImageBlocks pointing at the newly
// materialised BMP path so the next render pulls fresh ResolvedKind.
bool consumeRefreshSignal();

}  // namespace pendingImage
}  // namespace snapix
