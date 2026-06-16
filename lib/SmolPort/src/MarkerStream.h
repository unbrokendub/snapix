#pragma once

// =============================================================================
// MarkerStream — minimal event-based reader for byte-marker styled text.
//
// Walks a byte stream containing plain UTF-8 text interleaved with
// `kMarker`-prefixed style/structure markers (see Markers.h).  Emits
// callbacks via the `MarkerObserver` interface so the caller doesn't
// have to write a switch over every marker tag.
//
// The reader is purely byte-streaming — no internal buffering, no
// allocation, no assumption about the size of the input.  Feed it
// chunks via `feed()` and it'll route text runs and markers to the
// observer as they're decoded.  Designed for ESP32-C3's static-
// workspace pattern: zero heap activity, fixed-size internal state.
//
// Heritage: protocol from smol-epub's `html_strip.rs`, observer pattern
// borrowed from ioma8/cool's `paginator.rs::PaginationObserver` and
// adapted to C++.
// =============================================================================

#include <cstddef>
#include <cstdint>

#include "Markers.h"

namespace snapix::smolport {

// Status returned by the observer callbacks.  Returning `Stop` aborts
// the stream walk early — useful for "find the page that contains
// this anchor" passes that don't need to read past the target.
enum class ObserverStatus : uint8_t {
  Continue,
  Stop,
};

// Observer interface — subclass and override the events you care about.
// All callbacks default to no-op so a renderer that only cares about
// `onText` can ignore the rest.
class MarkerObserver {
 public:
  virtual ~MarkerObserver() = default;

  // Plain text run (UTF-8, no markers inside).  `len` is the run length
  // in bytes.  The pointer is valid only for the duration of the call —
  // the observer must copy bytes it wants to retain.
  virtual ObserverStatus onText(const uint8_t* /*text*/, size_t /*len*/) {
    return ObserverStatus::Continue;
  }

  // Inline style toggles.  Fired exactly once per matched on/off pair
  // in the stream; nested same-style markers are flattened by the
  // stripper before we see them.
  virtual ObserverStatus onBoldStart()    { return ObserverStatus::Continue; }
  virtual ObserverStatus onBoldEnd()      { return ObserverStatus::Continue; }
  virtual ObserverStatus onItalicStart()  { return ObserverStatus::Continue; }
  virtual ObserverStatus onItalicEnd()    { return ObserverStatus::Continue; }
  // v3.6.0 — superscript / subscript (smaller font, raised / lowered).
  virtual ObserverStatus onSuperStart()   { return ObserverStatus::Continue; }
  virtual ObserverStatus onSuperEnd()     { return ObserverStatus::Continue; }
  virtual ObserverStatus onSubStart()     { return ObserverStatus::Continue; }
  virtual ObserverStatus onSubEnd()       { return ObserverStatus::Continue; }

  // Block toggles.  `headingLevel` is 1..6; the stripper emits `2` when
  // unsure.  Quote has no level.
  virtual ObserverStatus onHeadingStart(uint8_t /*level*/) { return ObserverStatus::Continue; }
  virtual ObserverStatus onHeadingEnd()                    { return ObserverStatus::Continue; }
  virtual ObserverStatus onQuoteStart()                    { return ObserverStatus::Continue; }
  virtual ObserverStatus onQuoteEnd()                      { return ObserverStatus::Continue; }

  // v2.0.145 — Indent / Center block markers.  Indent stacks (paginator
  // tracks depth); Center is a simple on/off flag.
  virtual ObserverStatus onIndentStart()  { return ObserverStatus::Continue; }
  virtual ObserverStatus onIndentEnd()    { return ObserverStatus::Continue; }
  virtual ObserverStatus onCenterStart()  { return ObserverStatus::Continue; }
  virtual ObserverStatus onCenterEnd()    { return ObserverStatus::Continue; }

  // Structural breaks — no payload.
  virtual ObserverStatus onLineBreak()      { return ObserverStatus::Continue; }
  virtual ObserverStatus onParagraphBreak() { return ObserverStatus::Continue; }
  virtual ObserverStatus onPageBreak()      { return ObserverStatus::Continue; }
  virtual ObserverStatus onThematicBreak()  { return ObserverStatus::Continue; }

  // Payload-carrying markers.  `data` is valid only for the duration of
  // the call.  For image references `data` is the EPUB-relative path;
  // for anchors it's the anchor id string.
  virtual ObserverStatus onImageRef(const uint8_t* /*path*/, uint16_t /*len*/) {
    return ObserverStatus::Continue;
  }
  virtual ObserverStatus onAnchor(const uint8_t* /*id*/, uint16_t /*len*/) {
    return ObserverStatus::Continue;
  }

  // Called once when the stream walker reaches end-of-input cleanly
  // (no truncation, no premature stop).  Override to flush any final
  // state (e.g. emit the last partial page).
  virtual ObserverStatus onStreamEnd() { return ObserverStatus::Continue; }
};

// =============================================================================
// MarkerStreamReader — stateful byte-stream walker.
//
// Construct once, call `feed(bytes, len)` with chunks from the cache
// or inflate output.  Internally maintains the parse-state machine
// across chunk boundaries (marker straddles, payload length straddles,
// payload-body straddles).  Call `finish()` at end-of-input to fire
// `onStreamEnd`.
//
// State size: ~32 bytes per instance.  Suitable for stack or BSS.
// =============================================================================
class MarkerStreamReader {
 public:
  // Max payload size for kImageRef / kAnchor.  Anchors are typically
  // 8-32 bytes; image refs are filenames up to ~128 bytes.  Caps at
  // 512 to keep this reader's internal buffer bounded.
  static constexpr uint16_t kMaxPayloadBytes = 512;

  explicit MarkerStreamReader(MarkerObserver& observer) : observer_(observer) {}

  // Feed a chunk of bytes.  Returns false if the observer asked to
  // stop or if a malformed marker was encountered.
  bool feed(const uint8_t* data, size_t len);

  // Call at end-of-input.  Returns false if onStreamEnd asked to stop.
  bool finish();

  // Reset to initial state — useful for reusing the reader across
  // multiple chapter streams without reallocating.
  void reset();

 private:
  enum class State : uint8_t {
    InText,          // reading plain text bytes
    InMarkerEscape,  // just saw kMarker, awaiting tag byte
    InPayloadLenLo,  // tag was payload-carrying, awaiting length lo
    InPayloadLenHi,  //                                       length hi
    InPayloadBody,   // accumulating payload bytes
  };

  MarkerObserver& observer_;
  State state_ = State::InText;
  uint8_t pendingTag_ = 0;
  uint16_t payloadLen_ = 0;
  uint16_t payloadRecvd_ = 0;
  uint8_t payloadBuf_[kMaxPayloadBytes] = {};

  // Walk a contiguous run of plain-text bytes (no marker escape inside).
  // Returns position of next non-text byte or end-of-input.
  size_t walkText(const uint8_t* data, size_t len);

  // Dispatch a marker tag.  Sets state to InPayloadLenLo if the tag
  // carries payload, otherwise emits the event immediately and returns
  // to InText.
  bool dispatchTag(uint8_t tag);

  // Emit the accumulated payload event to the observer.
  bool emitPayloadEvent();
};

}  // namespace snapix::smolport
