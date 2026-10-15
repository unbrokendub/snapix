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
// All other text goes through Utf8TextFilter (TextHygiene.h), the same gate
// HtmlStripper applies: invisible format characters (BOM, soft hyphen, ZWSP,
// bidi controls, …) and C0 controls are dropped, C1 controls are read as
// windows-1252, U+2028/2029 and VT/FF become a space; everything else passes
// through unchanged.  A dropped character is not text — it neither resolves a
// pending newline run nor counts as content (a BOM before leading blank lines
// leaves them suppressed).  Inter-word whitespace (space, tab) passes through
// as text — StreamingPaginator::isAsciiSpace treats space/tab/\n/\r as word
// separators, so the paginator handles word breaking.  Literal 0x01 bytes (the
// marker escape) are self-doubled per the protocol.
//
// Same shape as HtmlStripper (sink + feed/finish/reset) so it can drive the
// shared markerizeStream() loop without any HtmlStripper dependency.
// =============================================================================

#include <cstddef>
#include <cstdint>

#include "HtmlStripper.h"  // HtmlStripperSink
#include "TextHygiene.h"   // Utf8TextFilter

namespace snapix::smolport {

class TxtStripper {
 public:
  // `startPendingBreak` (v3.9.0) — begin as if a paragraph break is pending, so
  // the first emitted text is preceded by kParagraphBreak.  Used by chunked
  // (lazy) markerize: each chunk after the first is split at a paragraph
  // boundary, so it must re-emit the break the previous chunk left pending.
  // This makes N independent paragraph-aligned chunks concatenate byte-for-byte
  // identical to one continuous markerize.
  explicit TxtStripper(HtmlStripperSink& sink, bool reflow = false, bool startPendingBreak = false)
      : sink_(sink), reflow_(reflow), seenContent_(startPendingBreak), forceBreak_(startPendingBreak) {}

  // Feed an arbitrary chunk of bytes.  Safe to call repeatedly; the
  // paragraph-collapse state and a UTF-8 sequence split across calls survive
  // chunk boundaries.  Returns the number of bytes consumed (== len unless the
  // sink's shouldStop() latched, for cooperative early-exit — mirrors
  // HtmlStripper::feed).
  size_t feed(const uint8_t* data, size_t len);

  // End of input.  Trailing newlines need no trailing paragraph break; only a
  // truncated UTF-8 sequence still held by the filter is passed through.
  void finish();

  // Reset for a fresh document.
  void reset() {
    filter_.reset();
    seenContent_ = false;
    newlinesPending_ = 0;
    forceBreak_ = false;
  }

 private:
  // One filtered unit from Utf8TextFilter: a newline, a CR, or text.
  void put(const uint8_t* p, size_t n);

  HtmlStripperSink& sink_;
  Utf8TextFilter filter_;
  bool reflow_ = false;          // single '\n' → space (true) vs paragraph break (false)
  bool seenContent_ = false;     // emitted any text yet? (suppresses a leading break)
  uint16_t newlinesPending_ = 0; // newlines seen since the last text byte (capped)
  bool forceBreak_ = false;      // emit a kParagraphBreak before the next text (chunk resume)
};

}  // namespace snapix::smolport
