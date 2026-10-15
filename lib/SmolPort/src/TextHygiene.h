#pragma once

// =============================================================================
// TextHygiene — the per-code-point text gate shared by the strippers.
//
// The reader fonts have no glyphs for format characters (soft hyphen, ZWSP,
// BOM, bidi controls, …) or for C0/C1 controls; left in the marker stream they
// render as '?' or split a word visibly.  HtmlStripper applies
// mapTextCodepoint() inside its own emit path (it also feeds decoded
// entities through it); TxtStripper and MarkdownStripper run their raw input
// through Utf8TextFilter.
// =============================================================================

#include <cstddef>
#include <cstdint>

#include "Markers.h"  // kMarker

namespace snapix::smolport {

// windows-1252 meaning of 0x80..0x9F (0 = undefined → dropped).  Browsers
// apply this to &#128;..&#159; — old converted books use &#151; for an em dash.
inline constexpr uint16_t kCp1252C1[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

// Zero-width / invisible format characters.  No glyph in the reader fonts —
// they rendered as '?' (or split words visibly).  Soft hyphen included: the
// paginator does its own hyphenation.
inline bool isInvisibleCodepoint(const uint32_t cp) {
  return cp == 0x00AD || cp == 0x061C || cp == 0x180E ||
         (cp >= 0x200B && cp <= 0x200F) ||   // ZWSP ZWNJ ZWJ LRM RLM
         (cp >= 0x202A && cp <= 0x202E) ||   // bidi embeddings / overrides
         (cp >= 0x2060 && cp <= 0x2064) ||   // word joiner, invisible math
         (cp >= 0x2066 && cp <= 0x206F) ||   // bidi isolates, deprecated
         (cp >= 0xFE00 && cp <= 0xFE0F) ||   // variation selectors
         cp == 0xFEFF ||                     // BOM / ZWNBSP
         (cp >= 0xE0000 && cp <= 0xE007F);   // tag characters
}

// What a non-ASCII code point becomes in the marker stream: 0 = dropped,
// otherwise the code point to emit (== cp when it passes unchanged).
inline uint32_t mapTextCodepoint(const uint32_t cp) {
  if (isInvisibleCodepoint(cp)) return 0;
  // C1 control — almost always windows-1252 text mis-decoded as Latin-1.
  if (cp >= 0x80u && cp <= 0x9Fu) return kCp1252C1[cp - 0x80u];
  if (cp == 0x2028u || cp == 0x2029u) return ' ';  // LINE / PARAGRAPH SEPARATOR
  return cp;
}

// Encode `cp` as UTF-8 into `out` (room for 4 bytes).  Returns the length.
inline uint8_t encodeUtf8(const uint32_t cp, uint8_t* out) {
  if (cp < 0x80u) {
    out[0] = static_cast<uint8_t>(cp);
    return 1;
  }
  if (cp < 0x800u) {
    out[0] = static_cast<uint8_t>(0xC0u | (cp >> 6));
    out[1] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
    return 2;
  }
  if (cp < 0x10000u) {
    out[0] = static_cast<uint8_t>(0xE0u | (cp >> 12));
    out[1] = static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
    return 3;
  }
  out[0] = static_cast<uint8_t>(0xF0u | (cp >> 18));
  out[1] = static_cast<uint8_t>(0x80u | ((cp >> 12) & 0x3Fu));
  out[2] = static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu));
  out[3] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
  return 4;
}

// Streaming UTF-8 gate for raw text (TXT, Markdown).  Bytes go in one at a
// time; what survives comes out through `out(const uint8_t* p, size_t n)`:
//   * a complete multi-byte character as ONE call (verbatim, remapped through
//     mapTextCodepoint, or nothing if it is dropped);
//   * a truncated sequence verbatim as one call, before the byte that broke it;
//   * any other byte as a 1-byte call.  '\t' '\n' '\r' and the 0x01 marker
//     escape are kept (the caller escapes 0x01); VT/FF become a space; the
//     remaining C0 controls and DEL are dropped.
// A sequence split across feed() calls is held here, so chunked input yields
// the same output as one pass.  flush() at end of input.
class Utf8TextFilter {
 public:
  template <typename Out>
  void push(const uint8_t b, Out&& out) {
    if (need_ > 0) {
      if ((b & 0xC0u) == 0x80u) {
        buf_[len_++] = b;
        if (--need_ == 0) {
          const uint8_t n = len_;
          len_ = 0;
          emitSequence(n, out);
        }
        return;
      }
      flush(out);  // truncated sequence — pass it through, then handle `b`
    }
    if (b >= 0xC2u && b <= 0xF4u) {
      buf_[0] = b;
      len_ = 1;
      need_ = b >= 0xF0u ? 3 : (b >= 0xE0u ? 2 : 1);
      return;
    }
    if (b < 0x20u || b == 0x7Fu) {
      if (b == '\t' || b == '\n' || b == '\r' || b == kMarker) {
        out(&b, 1);
      } else if (b == 0x0Bu || b == 0x0Cu) {
        const uint8_t sp = ' ';
        out(&sp, 1);
      }
      return;
    }
    out(&b, 1);
  }

  // Pass a pending (truncated) sequence through verbatim.
  template <typename Out>
  void flush(Out&& out) {
    if (len_ == 0) return;
    const uint8_t n = len_;
    len_ = 0;
    need_ = 0;
    out(buf_, n);
  }

  void reset() {
    len_ = 0;
    need_ = 0;
  }

 private:
  template <typename Out>
  void emitSequence(const uint8_t n, Out& out) {
    uint32_t cp;
    if (n == 2) {
      cp = ((buf_[0] & 0x1Fu) << 6) | (buf_[1] & 0x3Fu);
    } else if (n == 3) {
      cp = ((buf_[0] & 0x0Fu) << 12) | ((buf_[1] & 0x3Fu) << 6) | (buf_[2] & 0x3Fu);
    } else {
      cp = ((buf_[0] & 0x07u) << 18) | ((buf_[1] & 0x3Fu) << 12) |
           ((buf_[2] & 0x3Fu) << 6) | (buf_[3] & 0x3Fu);
    }
    const uint32_t mapped = mapTextCodepoint(cp);
    if (mapped == cp) {
      out(buf_, n);
    } else if (mapped != 0) {
      uint8_t enc[4];
      out(enc, encodeUtf8(mapped, enc));
    }
  }

  uint8_t buf_[4] = {};
  uint8_t len_ = 0;   // bytes held in buf_
  uint8_t need_ = 0;  // continuation bytes still expected
};

}  // namespace snapix::smolport
