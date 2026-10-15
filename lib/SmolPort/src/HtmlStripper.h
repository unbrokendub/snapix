#pragma once

// =============================================================================
// SmolPort HtmlStripper — converts an HTML/XHTML byte stream into the SmolPort
// byte-marker styled-text format (see Markers.h).
//
// Heritage: ported from hansmrtn/smol-epub `html_strip.rs` (MIT/Apache-2.0).
// Same convergent design as TrustyReader and ioma8/cool — stream-oriented
// linear-scan parser, no DOM, no Expat.  Output is a sequence of UTF-8 text
// bytes (passing through unchanged) interleaved with marker sequences
// `[0x01, tag, (payload)…]`.  Literal 0x01 bytes in text are escaped as
// `[0x01, 0x01]` per the protocol so the byte stream is self-synchronising.
//
// Tag → marker dispatch:
//   <b>, <strong>, </b>, </strong>      → kBoldOn / kBoldOff
//   <i>, <em>, </i>, </em>              → kItalicOn / kItalicOff
//   <h1>..<h6>, </h1>..</h6>            → kHeadingOn + '1'..'6' / kHeadingOff
//   <blockquote>, </blockquote>         → kQuoteOn / kQuoteOff
//   </p>                                 → kParagraphBreak (after the text)
//   <br>, <br/>                          → kLineBreak
//   <hr>, <hr/>                          → kBreak
//   <img src="…">, <image (xlink:)href>  → kImageRef + len-prefixed src
//   <a id="…">                           → kAnchor + len-prefixed id
//   <script>…</script>, <style>…</style> → body suppressed (no emit)
//   <title>, SVG <desc>/<metadata>       → body suppressed (HTML mode)
//   everything else (div, span, ...)    → silently skipped (text preserved)
//   <x/> (self-closing)                  → open dispatch + close dispatch
//
// Entities (Phase 3c, v3.11.12):
//   Named:  the full HTML 4 set (+ &apos;) — Latin-1, Greek, math, arrows.
//   Numeric: &#NNN; (decimal), &#xHH; / &#XHH; (hex) → UTF-8; 0x80–0x9F are
//            remapped through windows-1252 like browsers do (&#151; → —).
//   A '&' that does not start a known entity is emitted LITERALLY (pre-v3.11.12
//   it swallowed text up to the next ';' or '<' — "Tom & Jerry" → "Tom").
//
// Text hygiene (v3.11.12): invisible format characters (soft hyphen, ZWSP,
// ZWJ/ZWNJ, LRM/RLM, bidi controls, word joiner, BOM, variation selectors)
// and C0/C1 control characters are dropped — the renderer has no glyph for
// them and drew '?'.  A '<' that cannot open a tag is literal text.
//
// Phase 3a/b/c/d status: text passthrough, tag-to-marker dispatch, HTML
// entity decoding, attribute extraction for <img>/<a>, and raw-body
// suppression for <script>/<style> are all implemented.  Integration
// into the EPUB chapter parser is Phase 3e.
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace snapix::smolport {

// Output sink for HtmlStripper.  Receives the byte-marker stream in chunks.
// `len` is always > 0.  The sink owns where the bytes accumulate
// (in-memory buffer, file write, network send, etc.).
class HtmlStripperSink {
 public:
  virtual ~HtmlStripperSink() = default;
  virtual void emit(const uint8_t* data, size_t len) = 0;

  // v2.0.97: cooperative-stop signal for callers that want the stripper
  // to bail mid-feed.  Returning `true` makes `HtmlStripper::feed` exit
  // at the next byte boundary; the return value of `feed` tells the
  // caller how many bytes were actually consumed so they can rewind
  // their source cursor.  Default `false` keeps the legacy API
  // (synchronous-drain) untouched.
  //
  // Motivation: the byte-marker pipeline used by `ChapterHtmlSlimParser`
  // under SNAPIX_SMOL_HTML emits markers to `MarkerStreamReader` →
  // observer chain.  When the observer reports `Stop`, the reader
  // returns `false` and the downstream sink can latch the stop.  But
  // without this hook the stripper kept processing the rest of the
  // chunk, emitting markers that landed in a latched-and-dropping
  // sink; those events were lost forever and the parser's state went
  // out of sync with the stripper's byte cursor → "resume parser
  // produced 0 pages" bug.  Returning `true` here lets the stripper
  // halt the byte loop cleanly and the caller rewind to the exact
  // last-consumed byte.
  virtual bool shouldStop() const { return false; }
};

class HtmlStripper {
 public:
  // Source format / dispatch flavour.  The XML lexer is identical between
  // modes — only the tag→marker dispatch tables, attribute-capture rules,
  // and raw-body suppression set differ.  Phase 7 added Fb2 mode.
  enum class Mode : uint8_t { Html, Fb2 };

  explicit HtmlStripper(HtmlStripperSink& sink, Mode mode = Mode::Html);

  // Feed an arbitrary chunk of bytes.  Safe to call repeatedly;
  // internal state machine survives chunk boundaries.
  //
  // Returns the number of bytes actually consumed.  Normally equals
  // `len`; less when the sink's `shouldStop()` returned `true` mid-loop
  // (cooperative early-exit — see HtmlStripperSink::shouldStop for
  // motivation).  Callers that don't use the stop hook can ignore the
  // return value.
  size_t feed(const uint8_t* data, size_t len);

  // Signal end of input.  Flushes a trailing literal '&…' / '<' and any
  // buffered UTF-8 bytes.  Must be called once after the last feed().
  void finish();

  // Reset for a fresh document.
  void reset();

 private:
  // ---------------------------------------------------------------------------
  // State machine.  See HtmlStripper.cpp for transitions.
  // ---------------------------------------------------------------------------
  enum class State : uint8_t {
    Text,             // emitting plain text bytes
    TagOpen,          // saw '<'
    TagOpenBang,      // saw '<!'
    TagOpenBangDash,  // saw '<!-'
    TagName,          // collecting tag name characters
    AttrPreName,      // skipping whitespace before next attr name or '>'
    AttrName,         // collecting attr name
    AttrPreValue,     // saw '=', skipping whitespace before value
    AttrValueQuoted,  // inside "..."  or '...'
    AttrValueUnq,     // bare value, until whitespace or '>'
    CommentBody,
    CommentDash1,
    CommentDash2,
    EntityBody,
    RawBody,          // inside <script>/<style>, suppress emit
    RawBodyLT,        // saw '<' inside raw body
    RawBodyLTSlash,   // saw '</' inside raw body
    RawBodyTagName,   // collecting raw-body inner-tag name
    RawBodyAfter,     // confirmed raw-body close, scan to '>'
  };

  HtmlStripperSink& sink_;
  State state_;
  Mode  mode_;

  // Tag-name buffer.  Sized for the longest tag we care about
  // ("blockquote" = 10 chars) plus headroom.  Names longer than this
  // overflow into the no-op "unknown tag" path.
  static constexpr uint8_t kTagBufSize = 16;
  uint8_t tagName_[kTagBufSize];
  uint8_t tagNameLen_;
  bool    isEndTag_;
  // v3.11.12 — '/' seen right before '>' (`<p/>`, `<b/>`, `<script …/>`).
  // A self-closing tag dispatches its close too and never opens a raw body.
  bool    selfClosing_;

  // Attribute name buffer.  Largest we look up is "src" (3) / "id" (2);
  // 16 covers any realistic attribute we'd want to capture later.
  static constexpr uint8_t kAttrNameBufSize = 16;
  uint8_t attrName_[kAttrNameBufSize];
  uint8_t attrNameLen_;
  uint8_t attrQuote_;        // 0 = unquoted, else '"' or '\''
  bool    captureCurrent_;   // current attr value goes into payload_

  // Entity-name buffer (Phase 3c).
  static constexpr uint8_t kEntityBufSize = 16;
  uint8_t entityBuf_[kEntityBufSize];
  uint8_t entityLen_;

  // v3.11.12 — pending multi-byte UTF-8 sequence.  Text is decoded one code
  // point at a time so invisible format characters can be dropped and C1
  // controls remapped; a malformed sequence passes through verbatim.
  uint8_t utf8Buf_[4];
  uint8_t utf8Len_;
  uint8_t utf8Need_;

  // Payload buffer for `id` / `src` attribute values.  512 bytes is enough
  // for any realistic EPUB image path or anchor id.  Truncates silently
  // beyond that — renderer-side will fail to resolve, matching the v2.x
  // robustness profile.
  //
  // The buffer is reused for whichever attribute is currently being
  // captured.  We emit the corresponding marker as soon as the value
  // ends (not at tag close), so a single tag with both `id` and `src`
  // (e.g. <img id="fig1" src="x.jpg"/>) emits both markers, in source
  // order.
  static constexpr uint16_t kPayloadBufSize = 512;
  uint8_t  payload_[kPayloadBufSize];
  uint16_t payloadLen_;

  // Which kind of attribute the current value belongs to.
  //   None — value not captured (most attrs).
  //   Id   — `id="…"` on any tag — emit kAnchor.
  //   Src  — `src="…"` specifically on `<img>` — emit kImageRef.
  enum class AttrKind : uint8_t { None, Id, Src };
  AttrKind captureKind_;

  // Raw-body mode tracking — suppress emit + look for matching close tag.
  // Html mode: script / style.
  // Fb2  mode: binary / description / stylesheet.
  enum class RawBodyKind : uint8_t {
    None,
    Script, Style,                      // HTML
    Title, SvgDesc, SvgMetadata,        // HTML (v3.11.12)
    Binary, Description, Stylesheet,    // FB2
  };
  RawBodyKind rawBody_;
  // Sized for "description" (11) and "stylesheet" (10) with headroom.
  static constexpr uint8_t kRawBodyTagBufSize = 16;
  uint8_t rawBodyTagBuf_[kRawBodyTagBufSize];
  uint8_t rawBodyTagBufLen_;

  // ---------------------------------------------------------------------------
  // Emit helpers.
  // ---------------------------------------------------------------------------
  void emitChar(uint8_t b);                 // text byte, escaping 0x01
  void emitMarker(uint8_t tag);             // [01, tag]
  void emitPayloadMarker(uint8_t tag);      // [01, tag, len_lo, len_hi, data]
  void emitRaw(const uint8_t* data, size_t len);  // flushUtf8 + sink emit
  void emitUtf8(uint32_t codepoint);
  void emitCodepoint(uint32_t cp, const uint8_t* bytes, uint8_t n);
  void flushUtf8();                         // pass a partial sequence through
  void flushEntityLiteral();                // '&' + buffered name, verbatim

  // ---------------------------------------------------------------------------
  // Dispatch helpers.
  // ---------------------------------------------------------------------------
  void dispatchTag();        // inline / block style markers (Phase 3b)
  bool dispatchEntity(bool terminated);  // decode + emit; false = not an entity
  void onAttrValueComplete();// emits kAnchor/kImageRef if captured (Phase 3d)
  void onTagComplete();      // emits style markers, transitions out of tag
  bool isRawBodyTag() const; // current tag name opens a suppressed body
  void beginAttrValue();     // decide whether the value is captured
};

}  // namespace snapix::smolport
