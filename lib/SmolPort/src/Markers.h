#pragma once

// =============================================================================
// SmolPort byte-marker protocol — Snapix v3.0 styled-text stream format.
//
// Heritage: protocol identical to hansmrtn/smol-epub's `html_strip.rs`
// (MIT/Apache-2.0).  Adopted because three independent embedded-Rust
// e-reader projects converged on this byte-stream design for ESP32-class
// memory budgets (smol-epub, TrustyReader, ioma8/cool) and the design
// avoids the heap-fragmentation class of bugs that plagued Snapix v2.x.
//
// Encoding:
//   * Plain text bytes pass through unchanged (UTF-8 OK).
//   * A marker is the literal byte 0x01 followed by one tag byte.
//   * Some markers carry payload (image refs) — payload length is
//     explicit in the marker (next byte after tag).
//
// Why 0x01 as escape: it's a C0 control char that never appears in
// well-formed EPUB chapter text (Calibre, KDP, hand-authored).  Should
// the upstream HTML actually contain a U+0001 character (extremely rare
// — perhaps in a fiction excerpt that quotes binary data) the HTML
// stripper escapes it as `[0x01, 0x01]` (self-doubled marker).  See
// `MarkerStream::isMarkerEscape()`.
//
// Renderer behaviour:
//   * Walk the byte stream linearly.
//   * On byte 0x01: read next byte → switch to marker handler.
//   * On any other byte: append to current text run.
//
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace snapix::smolport {

// The escape byte that introduces a 2-byte (or longer) marker.
constexpr uint8_t kMarker = 0x01;

// -----------------------------------------------------------------------------
// Inline style markers — toggle on/off within a paragraph.
// -----------------------------------------------------------------------------
// Bold on / off.  Standard `<b>`, `<strong>` semantics.
constexpr uint8_t kBoldOn  = 'B';
constexpr uint8_t kBoldOff = 'b';

// Italic on / off.  Standard `<i>`, `<em>` semantics.
constexpr uint8_t kItalicOn  = 'I';
constexpr uint8_t kItalicOff = 'i';

// -----------------------------------------------------------------------------
// Block-level style markers — apply to subsequent lines until matching off.
// -----------------------------------------------------------------------------
// Heading on / off.  Centred + bold by default.  Heading level is encoded as
// a single ASCII digit byte immediately AFTER kHeadingOn (`'1'`..`'6'`); when
// the stripper isn't sure of the level it emits `'2'` (h2 equivalent).
constexpr uint8_t kHeadingOn  = 'H';
constexpr uint8_t kHeadingOff = 'h';

// Block quote on / off.  Indented + italic by default.
constexpr uint8_t kQuoteOn  = 'Q';
constexpr uint8_t kQuoteOff = 'q';

// v2.0.145 — Indent on / off.  Pushes the left margin in by a fixed
// indent step (StreamingPaginator: bodyLineHeight units, ~24-30 px) for
// as long as it's active.  Used by `<li>` items to visually offset the
// bullet+text from the surrounding paragraph margin.  May be nested:
// each Indent-On bumps the current indent by one step, each Indent-Off
// pops one step.  The streaming paginator caps depth at 4 levels so a
// pathological deeply-nested list doesn't push content past the right
// margin.
constexpr uint8_t kIndentOn  = 'D';  // (D)edent → indent on
constexpr uint8_t kIndentOff = 'd';

// v2.0.145 — Center on / off.  Lines fed while center is on get center-
// aligned at flush time instead of left-aligned + justified.  Used for
// `<center>` (HTML) and `<title>` (FB2 chapter heading).  Simple on/off
// boolean (no nesting).
constexpr uint8_t kCenterOn  = 'C';
constexpr uint8_t kCenterOff = 'c';

// -----------------------------------------------------------------------------
// Structural markers — break the flow, no on/off pairing.
// -----------------------------------------------------------------------------
// Thematic break / horizontal rule (`<hr>`).
constexpr uint8_t kBreak = 'S';

// Forced line break inside a paragraph (`<br>`).
constexpr uint8_t kLineBreak = 'L';

// Hard page break / chapter section start.  Renderer should flush the
// current page and begin a fresh one at the next visible content.
constexpr uint8_t kPageBreak = 'N';

// Paragraph break.  Renderer should treat as `\n\n` with paragraph
// spacing applied per RenderConfig.
constexpr uint8_t kParagraphBreak = 'P';

// -----------------------------------------------------------------------------
// Payload-carrying markers — followed by length-prefixed binary data.
// -----------------------------------------------------------------------------
// Inline image reference.  Encoding:
//   [0x01, 'M', len_lo, len_hi, path_bytes...]
//
// The 16-bit length is little-endian.  `path_bytes` is the EPUB-relative
// (post-normalised) image path; the renderer resolves it to a cached BMP
// via the same image-cache layer as v2.x.
constexpr uint8_t kImageRef = 'M';

// Anchor declaration — used for TOC / hyperlink targets.  Encoding:
//   [0x01, 'A', len_lo, len_hi, anchor_id_bytes...]
//
// The renderer / pagination layer records the current page number for
// each anchor id, building the same anchor map Snapix v2.x's
// `.anchors` file holds.
constexpr uint8_t kAnchor = 'A';

// -----------------------------------------------------------------------------
// Reserved range — markers above 0x7F are reserved for future use.
//
// Markers MUST be plain ASCII letters/digits so the byte stream stays
// printable enough for hex-dump debugging.  Lowercase = "off",
// uppercase = "on" for matched pairs.
// -----------------------------------------------------------------------------

// Convenience: write a marker (or marker pair) to any Print-like sink.
// Caller is responsible for output buffering / chunking.
template <typename SinkT>
inline void writeMarker(SinkT& sink, const uint8_t tag) {
  sink.write(kMarker);
  sink.write(tag);
}

template <typename SinkT>
inline void writeMarkerPayload(SinkT& sink, const uint8_t tag, const uint8_t* data,
                                const uint16_t len) {
  sink.write(kMarker);
  sink.write(tag);
  sink.write(static_cast<uint8_t>(len & 0xFF));
  sink.write(static_cast<uint8_t>((len >> 8) & 0xFF));
  if (len > 0 && data != nullptr) {
    sink.write(data, len);
  }
}

}  // namespace snapix::smolport
