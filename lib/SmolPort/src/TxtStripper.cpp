#include "TxtStripper.h"

#include "Markers.h"

namespace snapix::smolport {

size_t TxtStripper::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    filter_.push(data[i], [this](const uint8_t* p, const size_t n) { put(p, n); });

    // Cooperative early-exit (write failure / abort) — report bytes consumed.
    if (sink_.shouldStop()) return i + 1;
  }
  return len;
}

void TxtStripper::finish() {
  filter_.flush([this](const uint8_t* p, const size_t n) { put(p, n); });
}

void TxtStripper::put(const uint8_t* p, const size_t n) {
  // Newline: count it (deferred).  A run is resolved when the next text byte
  // arrives — so trailing newlines emit nothing, and the count distinguishes
  // a single newline (soft wrap in reflow mode) from a blank line (paragraph).
  if (n == 1 && p[0] == '\n') {
    if (seenContent_ && newlinesPending_ < 0xFFFF) ++newlinesPending_;
    return;
  }
  if (n == 1 && p[0] == '\r') return;  // CR (CRLF) — ignored; the '\n' drives the break.

  // Text.  A chunk-resume forced break takes priority over the newline run
  // (the previous chunk ended at a paragraph boundary with the break still
  // pending).
  if (forceBreak_) {
    const uint8_t br[2] = {kMarker, kParagraphBreak};
    sink_.emit(br, sizeof(br));
    forceBreak_ = false;
    newlinesPending_ = 0;
  } else if (newlinesPending_ > 0) {
    if (reflow_ && newlinesPending_ == 1) {
      // Hard-wrapped line continuation → join with a space.
      const uint8_t sp = ' ';
      sink_.emit(&sp, 1);
    } else {
      // Paragraph break: every newline run (non-reflow) or a blank line (reflow).
      const uint8_t br[2] = {kMarker, kParagraphBreak};
      sink_.emit(br, sizeof(br));
    }
    newlinesPending_ = 0;
  }

  if (n == 1 && p[0] == kMarker) {
    // Self-double a literal 0x01 so the stream stays self-synchronising.
    const uint8_t esc[2] = {kMarker, kMarker};
    sink_.emit(esc, sizeof(esc));
  } else {
    sink_.emit(p, n);
  }
  seenContent_ = true;
}

}  // namespace snapix::smolport
