#include "MarkerStream.h"

#include <cstring>

namespace snapix::smolport {

namespace {

// Returns true if the tag is a payload-carrying marker.
inline bool tagHasPayload(const uint8_t tag) {
  return tag == kImageRef || tag == kAnchor;
}

}  // namespace

void MarkerStreamReader::reset() {
  state_ = State::InText;
  pendingTag_ = 0;
  payloadLen_ = 0;
  payloadRecvd_ = 0;
  sourceBytesSeen_ = 0;
  pendingMarkerOffset_ = 0;
}

bool MarkerStreamReader::feed(const uint8_t* data, const size_t len) {
  const uint32_t feedBaseOffset = sourceBytesSeen_;
  // State-machine invariant (v2.0.101 architectural fix — diagnosed from
  // Phase 3e step 2's "perma-stuck after Stop" regression on spine=20):
  //
  // Every observer callback in our shim chain (HtmlStripperToReaderSink →
  // MarkerStreamShim) consumes the event BEFORE deciding whether to
  // signal Stop — i.e. by the time `observer_.onX(...)` returns Stop,
  // the side effects (characterData, emitStart, anchorMap.emplace_back,
  // etc.) have already happened.  Stop is purely a back-pressure signal
  // meaning "I've taken the event; please stop feeding me bytes".
  //
  // Pre-fix, this routine committed the State transition AFTER the
  // observer-Stop check.  When Stop fired, the next `feed()` call found
  // the machine still in `InPayloadBody` / `InMarkerEscape` / etc., with
  // `payloadRecvd_==payloadLen_` already satisfied — so it either
  // re-emitted the SAME event (double-fire on resume) or wedged in a
  // tight loop reading zero bytes per iteration.  Spine=20 page=6
  // observed the wedge: parser called `feed(empty, 0)` forever after a
  // page-boundary Stop because state stayed `InPayloadBody`.
  //
  // The fix is local: commit the State transition BEFORE the observer
  // call returns Stop.  The observer has already consumed the event,
  // so the machine is free to advance.  On Stop the next `feed()`
  // resumes from the natural follow-on state.
  size_t i = 0;
  while (i < len) {
    switch (state_) {
      case State::InText: {
        // Find the next escape byte (or end of input) — emit everything
        // before it as one text run, then transition on the escape.
        const size_t textEnd = walkText(data + i, len - i);
        if (textEnd > 0) {
          // v2.0.101: advance `i` BEFORE the observer call.  Observer
          // consumes the bytes regardless of return code (see semantic
          // note above) — if we leave `i` un-advanced on Stop, the
          // caller's next chunk re-feeds the same text bytes and we
          // double-emit them through onText / characterData.
          const uint8_t* runStart = data + i;
          i += textEnd;
          if (observer_.onText(runStart, textEnd) == ObserverStatus::Stop) {
            return false;
          }
        }
        if (i >= len) break;
        // data[i] must be kMarker — consume it and transition.
        pendingMarkerOffset_ = feedBaseOffset + static_cast<uint32_t>(i);
        ++i;
        state_ = State::InMarkerEscape;
        break;
      }

      case State::InMarkerEscape: {
        const uint8_t tag = data[i++];
        // Self-doubled marker → literal 0x01 byte in the source.  Emit
        // a one-byte text run and return to text-reading state.
        if (tag == kMarker) {
          // v2.0.101: commit transition BEFORE observer call (see top-
          // of-feed note).  Without this, Stop on a literal-0x01 byte
          // leaves state in InMarkerEscape; the next feed() reads the
          // following byte as a tag (data corruption).
          state_ = State::InText;
          if (observer_.onText(&tag, 1) == ObserverStatus::Stop) {
            return false;
          }
        } else {
          if (!dispatchTag(tag)) return false;
        }
        break;
      }

      case State::InPayloadLenLo: {
        payloadLen_ = data[i++];
        state_ = State::InPayloadLenHi;
        break;
      }

      case State::InPayloadLenHi: {
        payloadLen_ |= static_cast<uint16_t>(data[i++]) << 8;
        if (payloadLen_ > kMaxPayloadBytes) {
          // Malformed payload — too big for our buffer.  Abort: the
          // stream is corrupt and we can't safely skip ahead because
          // we don't know where the next valid marker starts.
          return false;
        }
        if (payloadLen_ == 0) {
          // v2.0.101: commit transition BEFORE emit.  Empty-payload
          // event still routes through the observer; Stop on an empty
          // payload used to leave state in InPayloadLenHi and the
          // next feed() would re-read a length byte as the wrong field.
          state_ = State::InText;
          if (!emitPayloadEvent()) return false;
        } else {
          payloadRecvd_ = 0;
          state_ = State::InPayloadBody;
        }
        break;
      }

      case State::InPayloadBody: {
        const uint16_t want = payloadLen_ - payloadRecvd_;
        const uint16_t avail = static_cast<uint16_t>(len - i);
        const uint16_t take = want < avail ? want : avail;
        std::memcpy(payloadBuf_ + payloadRecvd_, data + i, take);
        payloadRecvd_ += take;
        i += take;
        if (payloadRecvd_ == payloadLen_) {
          // v2.0.101: commit transition BEFORE emit.  This is the
          // dominant stuck-state in the wild — onImageRef / onAnchor
          // can return Stop because the parser's heap watermark
          // tripped or its page boundary fired, and pre-fix that left
          // state in InPayloadBody with `payloadRecvd_==payloadLen_`.
          // The next feed() loop iterated `take==0` and immediately
          // re-tried emit — but the observer's stop condition is
          // sticky, so we'd loop forever feeding zero bytes.  Spine=20
          // page=6 hot-extend wedge, fixed here.
          state_ = State::InText;
          if (!emitPayloadEvent()) return false;
        }
        break;
      }
    }
  }
  sourceBytesSeen_ += static_cast<uint32_t>(len);
  return true;
}

bool MarkerStreamReader::finish() {
  // Caller invariant: feed() should have been called with the entire
  // stream.  If we're mid-marker the stream is truncated — refuse to
  // pretend it finished cleanly.
  if (state_ != State::InText) {
    return false;
  }
  return observer_.onStreamEnd() != ObserverStatus::Stop;
}

size_t MarkerStreamReader::walkText(const uint8_t* data, const size_t len) {
  // Scan forward for the first escape byte.  memchr is the right tool
  // — it's ~5x faster than a hand-rolled byte loop on ESP32-C3 thanks
  // to the libc's word-aligned implementation.
  if (len == 0) return 0;
  const void* found = std::memchr(data, kMarker, len);
  if (found == nullptr) return len;  // entire chunk is plain text
  return static_cast<size_t>(static_cast<const uint8_t*>(found) - data);
}

bool MarkerStreamReader::dispatchTag(const uint8_t tag) {
  // Payload-carrying tags transition to length-read state; the rest
  // emit immediately and return to text.
  if (tagHasPayload(tag)) {
    pendingTag_ = tag;
    payloadLen_ = 0;
    payloadRecvd_ = 0;
    state_ = State::InPayloadLenLo;
    return true;
  }

  ObserverStatus rc = ObserverStatus::Continue;
  uint8_t headingLevel = 0;

  switch (tag) {
    case kBoldOn:         rc = observer_.onBoldStart(); break;
    case kBoldOff:        rc = observer_.onBoldEnd(); break;
    case kItalicOn:       rc = observer_.onItalicStart(); break;
    case kItalicOff:      rc = observer_.onItalicEnd(); break;
    case kSuperOn:        rc = observer_.onSuperStart(); break;  // v3.6.0
    case kSuperOff:       rc = observer_.onSuperEnd(); break;
    case kSubOn:          rc = observer_.onSubStart(); break;
    case kSubOff:         rc = observer_.onSubEnd(); break;

    case kHeadingOn:
      // Heading is followed by a single ASCII digit byte (level 1-6).
      // We don't have that byte yet — switch directly to InPayloadBody
      // with payloadLen_ pre-set to 1.  emitPayloadEvent dispatches to
      // onHeadingStart with the level decoded from payloadBuf_[0].
      //
      // (Original Phase 1 code routed through InPayloadLenLo which
      // overwrote payloadLen_ with the level byte itself, then read a
      // second byte as the high-byte of length — corrupting the state
      // machine.  Fixed in v2.0.88 when the test suite caught it.)
      pendingTag_ = tag;
      payloadLen_ = 1;
      payloadRecvd_ = 0;
      state_ = State::InPayloadBody;
      return true;

    case kHeadingOff:     rc = observer_.onHeadingEnd(); break;
    case kQuoteOn:        rc = observer_.onQuoteStart(); break;
    case kQuoteOff:       rc = observer_.onQuoteEnd(); break;
    case kIndentOn:       rc = observer_.onIndentStart(); break;
    case kIndentOff:      rc = observer_.onIndentEnd(); break;
    case kCenterOn:       rc = observer_.onCenterStart(); break;
    case kCenterOff:      rc = observer_.onCenterEnd(); break;
    case kLineBreak:      rc = observer_.onLineBreak(); break;
    case kParagraphBreak: rc = observer_.onParagraphBreak(); break;
    case kPageBreak:      rc = observer_.onPageBreak(); break;
    case kBreak:          rc = observer_.onThematicBreak(); break;

    default:
      // Unknown marker — corrupt stream.  Reject to avoid emitting
      // arbitrary text that the caller would misinterpret.
      return false;
  }

  (void)headingLevel;  // unused in this branch
  state_ = State::InText;
  return rc != ObserverStatus::Stop;
}

bool MarkerStreamReader::emitPayloadEvent() {
  ObserverStatus rc = ObserverStatus::Continue;
  switch (pendingTag_) {
    case kImageRef:
      rc = observer_.onImageRef(payloadBuf_, payloadLen_);
      break;
    case kAnchor:
      rc = observer_.onAnchorAt(payloadBuf_, payloadLen_, pendingMarkerOffset_);
      break;
    case kHeadingOn: {
      // payloadBuf_[0] holds the digit byte; treat '2' as default if
      // it's not ASCII '1'..'6'.
      const uint8_t lvlByte = payloadLen_ >= 1 ? payloadBuf_[0] : '2';
      const uint8_t lvl =
          (lvlByte >= '1' && lvlByte <= '6') ? static_cast<uint8_t>(lvlByte - '0') : 2;
      rc = observer_.onHeadingStart(lvl);
      break;
    }
    default:
      // Should never happen — dispatchTag only sets pendingTag_ for
      // known payload-carrying markers.
      return false;
  }
  pendingTag_ = 0;
  return rc != ObserverStatus::Stop;
}

}  // namespace snapix::smolport
