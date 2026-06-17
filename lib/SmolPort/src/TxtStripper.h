#pragma once

// =============================================================================
// SmolPort TxtStripper — converts a plain-text byte stream into the SmolPort
// byte-marker styled-text format (see Markers.h).
//
// Plain text carries no markup, so the only structural transform is paragraph
// detection.  The legacy PlainTextParser treated EVERY newline as a paragraph
// break (each source line became its own paragraph with spacing); this stripper
// preserves that "newline starts a new paragraph" model but COLLAPSES a run of
// consecutive newlines into a single kParagraphBreak — the legacy path emitted
// one paragraph gap per '\n', so a blank-line-separated file double-spaced; one
// break per run is the cleaner, equivalent result.
//
// All other bytes pass through unchanged (UTF-8 safe).  Inter-word whitespace
// (space, tab) passes through as text — StreamingPaginator::isAsciiSpace treats
// space/tab/\n/\r as word separators, so the paginator handles word breaking.
// Literal 0x01 bytes (the marker escape) are self-doubled per the protocol.
//
// Same shape as HtmlStripper (sink + feed/finish/reset) so it can drive the
// shared markerizeStream() loop without any HtmlStripper dependency.
// =============================================================================

#include <cstddef>
#include <cstdint>

#include "HtmlStripper.h"  // HtmlStripperSink

namespace snapix::smolport {

class TxtStripper {
 public:
  explicit TxtStripper(HtmlStripperSink& sink) : sink_(sink) {}

  // Feed an arbitrary chunk of bytes.  Safe to call repeatedly; the
  // paragraph-collapse state survives chunk boundaries.  Returns the number
  // of bytes consumed (== len unless the sink's shouldStop() latched, for
  // cooperative early-exit — mirrors HtmlStripper::feed).
  size_t feed(const uint8_t* data, size_t len);

  // End of input.  Trailing newlines need no trailing paragraph break, so
  // this is a no-op (kept for API parity with HtmlStripper).
  void finish() {}

  // Reset for a fresh document.
  void reset() {
    seenContent_ = false;
    pendingBreak_ = false;
  }

 private:
  HtmlStripperSink& sink_;
  bool seenContent_ = false;   // emitted any text yet? (suppresses a leading break)
  bool pendingBreak_ = false;  // newline(s) seen since the last text byte
};

}  // namespace snapix::smolport
