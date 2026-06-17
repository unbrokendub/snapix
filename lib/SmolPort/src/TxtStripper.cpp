#include "TxtStripper.h"

#include "Markers.h"

namespace snapix::smolport {

size_t TxtStripper::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const uint8_t b = data[i];

    // Newline: defer the paragraph break.  A run of consecutive '\n' (with
    // interspersed '\r') collapses to ONE break, emitted lazily when the next
    // text byte arrives.  Deferring also means trailing newlines emit nothing.
    if (b == '\n') {
      if (seenContent_) pendingBreak_ = true;
      continue;
    }
    if (b == '\r') continue;  // CR (CRLF) — ignored; the '\n' drives the break.

    // Any other byte is text content.  Emit the deferred paragraph break first.
    if (pendingBreak_) {
      const uint8_t br[2] = {kMarker, kParagraphBreak};
      sink_.emit(br, sizeof(br));
      pendingBreak_ = false;
    }
    if (b == kMarker) {
      // Self-double a literal 0x01 so the stream stays self-synchronising.
      const uint8_t esc[2] = {kMarker, kMarker};
      sink_.emit(esc, sizeof(esc));
    } else {
      sink_.emit(&b, 1);
    }
    seenContent_ = true;

    // Cooperative early-exit (write failure / abort) — report bytes consumed
    // so the caller can rewind, matching HtmlStripper::feed semantics.
    if (sink_.shouldStop()) return i + 1;
  }
  return len;
}

}  // namespace snapix::smolport
