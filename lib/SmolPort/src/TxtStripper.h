#pragma once

// =============================================================================
// SmolPort TxtStripper — converts a plain-text byte stream into the SmolPort
// byte-marker styled-text format (see Markers.h).
//
// Plain text carries no markup, so the only structural transform is paragraph
// detection.  Two modes (chosen by the caller from a sample of the file):
//
//   reflow = false  — every run of newlines → one kParagraphBreak.  Right for
//                     files where each line IS a paragraph (one long line per
//                     paragraph, no blank-line separators).
//
//   reflow = true   — a BLANK line (>= 2 newlines) → kParagraphBreak; a single
//                     newline → a space (soft wrap join).  Right for hard-
//                     wrapped text (short lines, blank-line paragraph breaks):
//                     the lines reflow into real paragraphs instead of each
//                     wrapped line becoming its own one-line paragraph.
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
  explicit TxtStripper(HtmlStripperSink& sink, bool reflow = false) : sink_(sink), reflow_(reflow) {}

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
    newlinesPending_ = 0;
  }

 private:
  HtmlStripperSink& sink_;
  bool reflow_ = false;          // single '\n' → space (true) vs paragraph break (false)
  bool seenContent_ = false;     // emitted any text yet? (suppresses a leading break)
  uint16_t newlinesPending_ = 0; // newlines seen since the last text byte (capped)
};

}  // namespace snapix::smolport
