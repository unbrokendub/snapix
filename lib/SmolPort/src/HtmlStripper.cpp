#include "HtmlStripper.h"

#include "Markers.h"

#include <cstring>

namespace snapix::smolport {

namespace {

// Tag-name / attr-name character: a-z, A-Z, 0-9, '-', '_', ':'.
inline bool isTagNameChar(const uint8_t b) {
  return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
         (b >= '0' && b <= '9') || b == '-' || b == '_' || b == ':';
}

// First char of an attribute name (XML Name first-char production subset).
inline bool isAttrNameStart(const uint8_t b) {
  return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
         b == '_' || b == ':';
}

inline bool isWhitespace(const uint8_t b) {
  return b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == '\f';
}

// Case-insensitive compare against a constant literal.
inline bool eqIc(const uint8_t* name, const uint8_t len, const char* s,
                 const size_t n) {
  if (len != n) return false;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t a = name[i] | 0x20u;
    const uint8_t c = static_cast<uint8_t>(s[i]) | 0x20u;
    if (a != c) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Phase 3c: named entity table.
// ---------------------------------------------------------------------------
struct NamedEntity {
  const char* name;
  uint8_t     nameLen;
  uint32_t    codepoint;
};

constexpr NamedEntity kNamedEntities[] = {
    // XML / HTML builtins.
    {"amp",     3, 0x0026}, {"lt",      2, 0x003C}, {"gt",      2, 0x003E},
    {"quot",    4, 0x0022}, {"apos",    4, 0x0027},
    // Whitespace / formatting.
    {"nbsp",    4, 0x00A0}, {"shy",     3, 0x00AD},
    {"thinsp",  6, 0x2009}, {"ensp",    4, 0x2002}, {"emsp",    4, 0x2003},
    // Typographic punctuation.
    {"mdash",   5, 0x2014}, {"ndash",   5, 0x2013}, {"hellip",  6, 0x2026},
    {"lsquo",   5, 0x2018}, {"rsquo",   5, 0x2019},
    {"ldquo",   5, 0x201C}, {"rdquo",   5, 0x201D},
    {"laquo",   5, 0x00AB}, {"raquo",   5, 0x00BB},
    {"bull",    4, 0x2022}, {"middot",  6, 0x00B7},
    {"dagger",  6, 0x2020}, {"Dagger",  6, 0x2021},
    // Symbols.
    {"copy",    4, 0x00A9}, {"reg",     3, 0x00AE}, {"trade",   5, 0x2122},
    {"deg",     3, 0x00B0}, {"sect",    4, 0x00A7}, {"para",    4, 0x00B6},
    // Currency.
    {"euro",    4, 0x20AC}, {"pound",   5, 0x00A3}, {"yen",     3, 0x00A5},
    {"cent",    4, 0x00A2},
    // Math.
    {"times",   5, 0x00D7}, {"divide",  6, 0x00F7}, {"plusmn",  6, 0x00B1},
    {"frac12",  6, 0x00BD}, {"frac14",  6, 0x00BC}, {"frac34",  6, 0x00BE},
    // Latin extended (French / Spanish / German).
    {"iexcl",   5, 0x00A1}, {"iquest",  6, 0x00BF}, {"szlig",   5, 0x00DF},
    {"agrave",  6, 0x00E0}, {"Agrave",  6, 0x00C0},
    {"eacute",  6, 0x00E9}, {"Eacute",  6, 0x00C9},
    {"egrave",  6, 0x00E8}, {"Egrave",  6, 0x00C8},
    {"ecirc",   5, 0x00EA}, {"Ecirc",   5, 0x00CA},
    {"icirc",   5, 0x00EE}, {"Icirc",   5, 0x00CE},
    {"ocirc",   5, 0x00F4}, {"Ocirc",   5, 0x00D4},
    {"ucirc",   5, 0x00FB}, {"Ucirc",   5, 0x00DB},
    {"ccedil",  6, 0x00E7}, {"Ccedil",  6, 0x00C7},
    {"ntilde",  6, 0x00F1}, {"Ntilde",  6, 0x00D1},
    {"auml",    4, 0x00E4}, {"Auml",    4, 0x00C4},
    {"ouml",    4, 0x00F6}, {"Ouml",    4, 0x00D6},
    {"uuml",    4, 0x00FC}, {"Uuml",    4, 0x00DC},
};

}  // namespace

// ===========================================================================
// Construction / lifecycle.
// ===========================================================================
HtmlStripper::HtmlStripper(HtmlStripperSink& sink, Mode mode)
    : sink_(sink),
      state_(State::Text),
      mode_(mode),
      tagNameLen_(0),
      isEndTag_(false),
      attrNameLen_(0),
      attrQuote_(0),
      captureCurrent_(false),
      entityLen_(0),
      payloadLen_(0),
      captureKind_(AttrKind::None),
      rawBody_(RawBodyKind::None),
      rawBodyTagBufLen_(0) {}

void HtmlStripper::reset() {
  state_ = State::Text;
  tagNameLen_ = 0;
  isEndTag_ = false;
  attrNameLen_ = 0;
  attrQuote_ = 0;
  captureCurrent_ = false;
  entityLen_ = 0;
  payloadLen_ = 0;
  captureKind_ = AttrKind::None;
  rawBody_ = RawBodyKind::None;
  rawBodyTagBufLen_ = 0;
}

void HtmlStripper::finish() {
  // No-op for now.  Future: emit trailing kParagraphBreak / close open scope.
}

// ===========================================================================
// Emit helpers.
// ===========================================================================
void HtmlStripper::emitChar(const uint8_t b) {
  if (b == kMarker) {
    const uint8_t pair[2] = {kMarker, kMarker};
    sink_.emit(pair, 2);
  } else {
    sink_.emit(&b, 1);
  }
}

void HtmlStripper::emitMarker(const uint8_t tag) {
  const uint8_t bytes[2] = {kMarker, tag};
  sink_.emit(bytes, 2);
}

void HtmlStripper::emitPayloadMarker(const uint8_t tag) {
  const uint8_t hdr[4] = {kMarker, tag,
                           static_cast<uint8_t>(payloadLen_ & 0xFFu),
                           static_cast<uint8_t>((payloadLen_ >> 8) & 0xFFu)};
  sink_.emit(hdr, 4);
  if (payloadLen_ > 0) sink_.emit(payload_, payloadLen_);
}

void HtmlStripper::emitUtf8(const uint32_t cp) {
  if (cp > 0x10FFFFu) return;
  uint8_t buf[4];
  uint8_t n;
  if (cp < 0x80u) {
    buf[0] = static_cast<uint8_t>(cp);
    n = 1;
  } else if (cp < 0x800u) {
    buf[0] = static_cast<uint8_t>(0xC0u | (cp >> 6));
    buf[1] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
    n = 2;
  } else if (cp < 0x10000u) {
    buf[0] = static_cast<uint8_t>(0xE0u | (cp >> 12));
    buf[1] = static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu));
    buf[2] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
    n = 3;
  } else {
    buf[0] = static_cast<uint8_t>(0xF0u | (cp >> 18));
    buf[1] = static_cast<uint8_t>(0x80u | ((cp >> 12) & 0x3Fu));
    buf[2] = static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu));
    buf[3] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
    n = 4;
  }
  for (uint8_t i = 0; i < n; ++i) emitChar(buf[i]);
}

// ===========================================================================
// Tag dispatch — inline / block style markers.
// Branches on `mode_` to support both HTML (Phase 3b) and FB2 (Phase 7)
// without duplicating the surrounding state machine.
// ===========================================================================
void HtmlStripper::dispatchTag() {
  if (tagNameLen_ == 0) return;
  if (mode_ == Mode::Fb2) {
    // ----- FB2 dispatch table -----
    if (eqIc(tagName_, tagNameLen_, "emphasis", 8)) {
      emitMarker(isEndTag_ ? kItalicOff : kItalicOn);
      return;
    }
    if (eqIc(tagName_, tagNameLen_, "strong", 6)) {
      emitMarker(isEndTag_ ? kBoldOff : kBoldOn);
      return;
    }
    if (eqIc(tagName_, tagNameLen_, "title", 5)) {
      // FB2 <title> appears inside <body> (book title) or <section>
      // (chapter / sub-chapter heading).  Without nesting tracking we
      // always emit heading level '2' — renderer can refine.
      // v2.0.145 — also wrap in kCenterOn/Off so chapter titles
      // visually centre as in the legacy ChapterHtmlSlimParser.
      if (isEndTag_) {
        emitMarker(kHeadingOff);
        emitMarker(kCenterOff);
      } else {
        emitMarker(kCenterOn);
        const uint8_t bytes[3] = {kMarker, kHeadingOn, '2'};
        sink_.emit(bytes, 3);
      }
      return;
    }
    if (eqIc(tagName_, tagNameLen_, "subtitle", 8)) {
      // v2.0.145 — subtitles also centered.
      if (isEndTag_) {
        emitMarker(kHeadingOff);
        emitMarker(kCenterOff);
      } else {
        emitMarker(kCenterOn);
        const uint8_t bytes[3] = {kMarker, kHeadingOn, '3'};
        sink_.emit(bytes, 3);
      }
      return;
    }
    if (eqIc(tagName_, tagNameLen_, "cite", 4)) {
      emitMarker(isEndTag_ ? kQuoteOff : kQuoteOn);
      return;
    }
    // v2.0.143: <epigraph> — opening quote from a book; render as block
    // quote (matches the legacy ChapterHtmlSlimParser visual).
    if (eqIc(tagName_, tagNameLen_, "epigraph", 8)) {
      emitMarker(isEndTag_ ? kQuoteOff : kQuoteOn);
      return;
    }
    // v2.0.143: <text-author> appears inside <epigraph>/<cite>; render
    // italic for the typical "— Имя Автора" attribution typography.
    // Also emit a paragraph break before/after to separate the author
    // line from the quote text.
    if (eqIc(tagName_, tagNameLen_, "text-author", 11)) {
      if (isEndTag_) {
        emitMarker(kItalicOff);
        emitMarker(kParagraphBreak);
      } else {
        emitMarker(kParagraphBreak);
        emitMarker(kItalicOn);
      }
      return;
    }
    if (eqIc(tagName_, tagNameLen_, "p", 1)) {
      if (isEndTag_) emitMarker(kParagraphBreak);
      return;
    }
    if (eqIc(tagName_, tagNameLen_, "empty-line", 10)) {
      if (!isEndTag_) emitMarker(kLineBreak);
      return;
    }
    if (eqIc(tagName_, tagNameLen_, "v", 1)) {
      // Verse line in poetry.
      if (!isEndTag_) emitMarker(kLineBreak);
      return;
    }
    // v2.0.143: FB2 poetry containers.  <poem> wraps a whole poem;
    // <stanza> wraps one stanza (group of verses).  Both produce
    // paragraph spacing around themselves so the poem reads as a
    // distinct visual block.
    if (eqIc(tagName_, tagNameLen_, "poem", 4)) {
      emitMarker(kParagraphBreak);
      return;
    }
    if (eqIc(tagName_, tagNameLen_, "stanza", 6)) {
      // Stanza spacing: paragraph break between stanzas so they don't
      // run together.
      emitMarker(kParagraphBreak);
      return;
    }
    if (eqIc(tagName_, tagNameLen_, "section", 7)) {
      // Section boundary → start of a new logical page.
      if (!isEndTag_) emitMarker(kPageBreak);
      return;
    }
    // <image>, <a>, <binary>, <description>, <stylesheet>: handled in
    // attribute capture / onTagComplete raw-body switching.
    return;
  }

  // ----- HTML dispatch table (existing Phase 3b) -----
  if (eqIc(tagName_, tagNameLen_, "b", 1) ||
      eqIc(tagName_, tagNameLen_, "strong", 6)) {
    emitMarker(isEndTag_ ? kBoldOff : kBoldOn);
    return;
  }
  if (eqIc(tagName_, tagNameLen_, "i", 1) ||
      eqIc(tagName_, tagNameLen_, "em", 2)) {
    emitMarker(isEndTag_ ? kItalicOff : kItalicOn);
    return;
  }
  // v3.6.0 — superscript / subscript.  Same `<sup>`/`<sub>` tag names in HTML
  // and FB2 (FB2 falls through to this common dispatch for inline styles), so
  // one entry covers both formats — footnote refs + formulas.
  if (eqIc(tagName_, tagNameLen_, "sup", 3)) {
    emitMarker(isEndTag_ ? kSuperOff : kSuperOn);
    return;
  }
  if (eqIc(tagName_, tagNameLen_, "sub", 3)) {
    emitMarker(isEndTag_ ? kSubOff : kSubOn);
    return;
  }
  if (tagNameLen_ == 2 && (tagName_[0] | 0x20u) == 'h' &&
      tagName_[1] >= '1' && tagName_[1] <= '6') {
    // v3.10.6 — HTML headings carry their text DIRECTLY (unlike FB2 <title>,
    // which wraps it in inner <p> that break on close).  Without a block break
    // around the heading, an EPUB chapter's <h1>ГЛАВА 15</h1><h2>title</h2>
    // collapsed inline with the running header and the first body paragraph —
    // "the whole chapter start on one line".  Isolate the heading as its own
    // block (paragraph break before + after) and centre it, matching the FB2
    // <title> appearance.  The leading break is a no-op-ish blank-line at a
    // fresh chapter top (acceptable breathing room); it never trips pageFull
    // (only kPageBreak does), so no empty page-0.
    if (isEndTag_) {
      emitMarker(kHeadingOff);
      emitMarker(kCenterOff);
      emitMarker(kParagraphBreak);
    } else {
      emitMarker(kParagraphBreak);
      emitMarker(kCenterOn);
      const uint8_t bytes[3] = {kMarker, kHeadingOn, tagName_[1]};
      sink_.emit(bytes, 3);
    }
    return;
  }
  if (eqIc(tagName_, tagNameLen_, "blockquote", 10)) {
    // v2.0.145 — also indent the block so the quote sits inside the
    // surrounding text column (matches the legacy renderer's "indented
    // italic block" appearance).
    if (isEndTag_) {
      emitMarker(kQuoteOff);
      emitMarker(kIndentOff);
    } else {
      emitMarker(kIndentOn);
      emitMarker(kQuoteOn);
    }
    return;
  }
  if (eqIc(tagName_, tagNameLen_, "p", 1)) {
    if (isEndTag_) emitMarker(kParagraphBreak);
    return;
  }
  if (eqIc(tagName_, tagNameLen_, "br", 2)) {
    if (!isEndTag_) emitMarker(kLineBreak);
    return;
  }
  if (eqIc(tagName_, tagNameLen_, "hr", 2)) {
    if (!isEndTag_) emitMarker(kBreak);
    return;
  }
  // v2.0.143: <div> — generic block container.  Common in modern EPUBs
  // as a paragraph wrapper around scene divisions, chapter intros, etc.
  // Treat as paragraph break on close so the visual block separation
  // matches a <p> equivalent.  Don't emit on open (would double-break
  // when div wraps a starting <p>).
  if (eqIc(tagName_, tagNameLen_, "div", 3)) {
    if (isEndTag_) emitMarker(kParagraphBreak);
    return;
  }
  // v2.0.143/145: <center> — legacy centered-block tag, still used in
  // older EPUBs for chapter titles / dedication pages.  v2.0.145 now
  // emits actual centering markers (kCenterOn/Off) so the paginator
  // center-aligns the lines, plus paragraph breaks to isolate the
  // centered block from surrounding paragraphs.
  if (eqIc(tagName_, tagNameLen_, "center", 6)) {
    if (isEndTag_) {
      emitMarker(kCenterOff);
      emitMarker(kParagraphBreak);
    } else {
      emitMarker(kParagraphBreak);
      emitMarker(kCenterOn);
    }
    return;
  }
  // v2.0.143/145: List containers <ul>/<ol> — paragraph break to
  // separate from surrounding text + indent ON for the whole list so
  // every item's bullet sits inside the indent.  On close: indent OFF
  // and another paragraph break.
  if (eqIc(tagName_, tagNameLen_, "ul", 2) ||
      eqIc(tagName_, tagNameLen_, "ol", 2)) {
    if (isEndTag_) {
      emitMarker(kIndentOff);
      emitMarker(kParagraphBreak);
    } else {
      emitMarker(kParagraphBreak);
      emitMarker(kIndentOn);
    }
    return;
  }
  // v2.0.143/145: <li> — list item.  On open: emit line break + bullet
  // glyph as inline text.  The bullet sits at the indent margin (set
  // by the enclosing <ul>/<ol>).  Bullet glyph: U+2022 (•) = E2 80 A2
  // UTF-8.  On close: no marker (next item or list close handles
  // separation).
  //
  // Limitation: numbered <ol> items still get bullets (proper numbering
  // needs a per-list counter state machine, deferred).
  if (eqIc(tagName_, tagNameLen_, "li", 2)) {
    if (!isEndTag_) {
      emitMarker(kLineBreak);
      static const uint8_t kBulletBytes[] = {0xE2, 0x80, 0xA2, ' '};
      sink_.emit(kBulletBytes, sizeof(kBulletBytes));
    }
    return;
  }
  // Unknown / unhandled tag — text content still emitted by outer loop.
}

// ===========================================================================
// Entity dispatch (Phase 3c).
// ===========================================================================
void HtmlStripper::dispatchEntity() {
  if (entityLen_ == 0) return;

  if (entityBuf_[0] == '#') {
    if (entityLen_ < 2) return;
    uint32_t cp = 0;
    if (entityBuf_[1] == 'x' || entityBuf_[1] == 'X') {
      if (entityLen_ < 3) return;
      for (uint8_t i = 2; i < entityLen_; ++i) {
        const uint8_t c = entityBuf_[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return;
        if (cp > (0x10FFFFu >> 4)) return;
        cp = (cp << 4) | d;
      }
    } else {
      for (uint8_t i = 1; i < entityLen_; ++i) {
        const uint8_t c = entityBuf_[i];
        if (c < '0' || c > '9') return;
        if (cp > (0x10FFFFu / 10)) return;
        cp = cp * 10 + static_cast<uint32_t>(c - '0');
      }
    }
    emitUtf8(cp);
    return;
  }

  for (const auto& e : kNamedEntities) {
    if (e.nameLen != entityLen_) continue;
    bool match = true;
    for (uint8_t i = 0; i < entityLen_; ++i) {
      if (e.name[i] != static_cast<char>(entityBuf_[i])) { match = false; break; }
    }
    if (match) {
      emitUtf8(e.codepoint);
      return;
    }
  }
  // Unknown — silently dropped.
}

// ===========================================================================
// Phase 3d: per-attribute and per-tag dispatch.
// ===========================================================================
// At attr-value-end we know the full payload bytes and the attribute name,
// so emit the corresponding marker inline (rather than buffering for tag
// close).  Order in the output stream then follows source order: e.g.
// `<h1 id="x">Title</h1>` emits kAnchor("x") then kHeadingOn'1' then text.
void HtmlStripper::onAttrValueComplete() {
  if (captureCurrent_ && payloadLen_ > 0) {
    if (captureKind_ == AttrKind::Id)        emitPayloadMarker(kAnchor);
    else if (captureKind_ == AttrKind::Src)  emitPayloadMarker(kImageRef);
  }
  captureCurrent_ = false;
  captureKind_ = AttrKind::None;
  attrNameLen_ = 0;
}

// At tag close we still need to:
//   - emit style markers (kBoldOn/Off etc.)
//   - enter raw-body suppression for <script>/<style>
// Payload markers (img/a) were already emitted by onAttrValueComplete.
void HtmlStripper::onTagComplete() {
  dispatchTag();
  if (!isEndTag_) {
    if (mode_ == Mode::Fb2) {
      // FB2 raw bodies: <binary> holds base64 image data, <description>
      // holds book metadata (title-info / publish-info), <stylesheet>
      // holds CSS.  None contribute to renderable text.
      if (eqIc(tagName_, tagNameLen_, "binary", 6)) {
        rawBody_ = RawBodyKind::Binary;
        state_ = State::RawBody;
        return;
      }
      if (eqIc(tagName_, tagNameLen_, "description", 11)) {
        rawBody_ = RawBodyKind::Description;
        state_ = State::RawBody;
        return;
      }
      if (eqIc(tagName_, tagNameLen_, "stylesheet", 10)) {
        rawBody_ = RawBodyKind::Stylesheet;
        state_ = State::RawBody;
        return;
      }
    } else {
      // HTML raw bodies.
      if (eqIc(tagName_, tagNameLen_, "script", 6)) {
        rawBody_ = RawBodyKind::Script;
        state_ = State::RawBody;
        return;
      }
      if (eqIc(tagName_, tagNameLen_, "style", 5)) {
        rawBody_ = RawBodyKind::Style;
        state_ = State::RawBody;
        return;
      }
    }
  }
  state_ = State::Text;
}

// ===========================================================================
// Feed loop — state machine.
// ===========================================================================
size_t HtmlStripper::feed(const uint8_t* data, const size_t len) {
  for (size_t i = 0; i < len; ++i) {
    // v2.0.97: cooperative-stop check.  See HtmlStripperSink::shouldStop
    // for the design motivation — callers that latch a stop signal on
    // the sink can have us bail at the next byte boundary so the byte
    // cursor and the parser's observed-event cursor stay aligned.
    if (sink_.shouldStop()) {
      return i;
    }
    const uint8_t b = data[i];
    switch (state_) {
      // ----- text -----
      case State::Text:
        if (b == '<') {
          state_ = State::TagOpen;
          // Clean slate: per-attr capture state will be recomputed when
          // each attribute's value starts.
          captureCurrent_ = false;
          captureKind_ = AttrKind::None;
          payloadLen_ = 0;
        } else if (b == '&') {
          state_ = State::EntityBody;
          entityLen_ = 0;
        } else {
          emitChar(b);
        }
        break;

      // ----- tag open prefix -----
      case State::TagOpen:
        if (b == '!') {
          state_ = State::TagOpenBang;
        } else if (b == '/') {
          isEndTag_ = true;
          tagNameLen_ = 0;
          state_ = State::TagName;
        } else if (b == '>') {
          state_ = State::Text;
        } else if (isTagNameChar(b)) {
          isEndTag_ = false;
          tagNameLen_ = 0;
          tagName_[tagNameLen_++] = b;
          state_ = State::TagName;
        } else {
          state_ = State::AttrPreName;  // weird; skip until '>'
        }
        break;

      case State::TagOpenBang:
        if (b == '-')      state_ = State::TagOpenBangDash;
        else if (b == '>') state_ = State::Text;
        else               state_ = State::AttrPreName;
        break;

      case State::TagOpenBangDash:
        if (b == '-')      state_ = State::CommentBody;
        else if (b == '>') state_ = State::Text;
        else               state_ = State::AttrPreName;
        break;

      // ----- tag name -----
      case State::TagName:
        if (isTagNameChar(b)) {
          if (tagNameLen_ < kTagBufSize) tagName_[tagNameLen_++] = b;
        } else if (b == '>') {
          onTagComplete();
        } else {
          state_ = State::AttrPreName;
        }
        break;

      // ----- attribute scanning -----
      case State::AttrPreName:
        if (b == '>') {
          onTagComplete();
        } else if (b == '/') {
          // Self-closing slash before '>': stay, will hit '>' next.
        } else if (isAttrNameStart(b)) {
          attrNameLen_ = 0;
          attrName_[attrNameLen_++] = b;
          state_ = State::AttrName;
        }
        // Whitespace / other chars: skipped.
        break;

      case State::AttrName:
        if (isTagNameChar(b)) {
          if (attrNameLen_ < kAttrNameBufSize) attrName_[attrNameLen_++] = b;
        } else if (b == '=') {
          state_ = State::AttrPreValue;
        } else if (b == '>') {
          // Boolean attribute (no value), tag ends.
          attrNameLen_ = 0;
          onTagComplete();
        } else {
          // Whitespace: end of name, no value seen yet.  Treat as boolean.
          attrNameLen_ = 0;
          state_ = State::AttrPreName;
        }
        break;

      case State::AttrPreValue:
        if (b == '"' || b == '\'') {
          attrQuote_ = b;
          // `id` on any non-end tag → kAnchor.
          // `src` specifically on <img> → kImageRef.
          captureKind_ = AttrKind::None;
          // `id` on raw-body tags (script/style/binary/description/
          // stylesheet) is internal-reference-only — do NOT emit kAnchor
          // for those (would dirty the anchor map with unreachable IDs).
          const bool isRawBodyTag =
              (mode_ == Mode::Html &&
               (eqIc(tagName_, tagNameLen_, "script", 6) ||
                eqIc(tagName_, tagNameLen_, "style", 5))) ||
              (mode_ == Mode::Fb2 &&
               (eqIc(tagName_, tagNameLen_, "binary", 6) ||
                eqIc(tagName_, tagNameLen_, "description", 11) ||
                eqIc(tagName_, tagNameLen_, "stylesheet", 10)));
          if (!isEndTag_) {
            if (!isRawBodyTag &&
                eqIc(attrName_, attrNameLen_, "id", 2)) {
              captureKind_ = AttrKind::Id;
            } else if (mode_ == Mode::Html &&
                       eqIc(attrName_, attrNameLen_, "src", 3) &&
                       eqIc(tagName_, tagNameLen_, "img", 3)) {
              captureKind_ = AttrKind::Src;
            } else if (mode_ == Mode::Fb2 &&
                       eqIc(attrName_, attrNameLen_, "l:href", 6) &&
                       eqIc(tagName_, tagNameLen_, "image", 5)) {
              captureKind_ = AttrKind::Src;  // emit as kImageRef
            }
          }
          captureCurrent_ = (captureKind_ != AttrKind::None);
          if (captureCurrent_) payloadLen_ = 0;
          state_ = State::AttrValueQuoted;
        } else if (b == '>') {
          attrNameLen_ = 0;
          onTagComplete();
        } else if (isWhitespace(b)) {
          // Skip; expect value next.
        } else {
          attrQuote_ = 0;
          captureKind_ = AttrKind::None;
          const bool isRawBodyTagUnq =
              (mode_ == Mode::Html &&
               (eqIc(tagName_, tagNameLen_, "script", 6) ||
                eqIc(tagName_, tagNameLen_, "style", 5))) ||
              (mode_ == Mode::Fb2 &&
               (eqIc(tagName_, tagNameLen_, "binary", 6) ||
                eqIc(tagName_, tagNameLen_, "description", 11) ||
                eqIc(tagName_, tagNameLen_, "stylesheet", 10)));
          if (!isEndTag_) {
            if (!isRawBodyTagUnq &&
                eqIc(attrName_, attrNameLen_, "id", 2)) {
              captureKind_ = AttrKind::Id;
            } else if (mode_ == Mode::Html &&
                       eqIc(attrName_, attrNameLen_, "src", 3) &&
                       eqIc(tagName_, tagNameLen_, "img", 3)) {
              captureKind_ = AttrKind::Src;
            } else if (mode_ == Mode::Fb2 &&
                       eqIc(attrName_, attrNameLen_, "l:href", 6) &&
                       eqIc(tagName_, tagNameLen_, "image", 5)) {
              captureKind_ = AttrKind::Src;  // emit as kImageRef
            }
          }
          captureCurrent_ = (captureKind_ != AttrKind::None);
          if (captureCurrent_) {
            payloadLen_ = 0;
            if (payloadLen_ < kPayloadBufSize) payload_[payloadLen_++] = b;
          }
          state_ = State::AttrValueUnq;
        }
        break;

      case State::AttrValueQuoted:
        if (b == attrQuote_) {
          onAttrValueComplete();
          state_ = State::AttrPreName;
        } else if (captureCurrent_ && payloadLen_ < kPayloadBufSize) {
          payload_[payloadLen_++] = b;
        }
        break;

      case State::AttrValueUnq:
        if (b == '>') {
          onAttrValueComplete();
          onTagComplete();
        } else if (isWhitespace(b)) {
          onAttrValueComplete();
          state_ = State::AttrPreName;
        } else if (captureCurrent_ && payloadLen_ < kPayloadBufSize) {
          payload_[payloadLen_++] = b;
        }
        break;

      // ----- comments -----
      case State::CommentBody:
        if (b == '-') state_ = State::CommentDash1;
        break;
      case State::CommentDash1:
        if (b == '-') state_ = State::CommentDash2;
        else          state_ = State::CommentBody;
        break;
      case State::CommentDash2:
        if (b == '>')      state_ = State::Text;
        else if (b == '-') state_ = State::CommentDash2;
        else               state_ = State::CommentBody;
        break;

      // ----- entities -----
      case State::EntityBody:
        if (b == ';') {
          dispatchEntity();
          state_ = State::Text;
        } else if (b == '<') {
          state_ = State::TagOpen;
        } else if (b == '&') {
          entityLen_ = 0;
        } else if (entityLen_ < kEntityBufSize) {
          entityBuf_[entityLen_++] = b;
        }
        break;

      // ----- raw body (script / style) -----
      case State::RawBody:
        if (b == '<') {
          state_ = State::RawBodyLT;
        }
        // All other bytes silently consumed.
        break;

      case State::RawBodyLT:
        if (b == '/')      state_ = State::RawBodyLTSlash;
        else if (b == '<') state_ = State::RawBodyLT;
        else               state_ = State::RawBody;
        break;

      case State::RawBodyLTSlash:
        if (isTagNameChar(b)) {
          rawBodyTagBufLen_ = 0;
          rawBodyTagBuf_[rawBodyTagBufLen_++] = b;
          state_ = State::RawBodyTagName;
        } else if (b == '<') {
          state_ = State::RawBodyLT;
        } else {
          state_ = State::RawBody;
        }
        break;

      case State::RawBodyTagName:
        if (isTagNameChar(b)) {
          if (rawBodyTagBufLen_ < sizeof(rawBodyTagBuf_)) {
            rawBodyTagBuf_[rawBodyTagBufLen_++] = b;
          }
        } else {
          // End of inner tag name — does it match the open?
          bool matchClose = false;
          switch (rawBody_) {
            case RawBodyKind::Script:
              matchClose = eqIc(rawBodyTagBuf_, rawBodyTagBufLen_, "script", 6);
              break;
            case RawBodyKind::Style:
              matchClose = eqIc(rawBodyTagBuf_, rawBodyTagBufLen_, "style", 5);
              break;
            case RawBodyKind::Binary:
              matchClose = eqIc(rawBodyTagBuf_, rawBodyTagBufLen_, "binary", 6);
              break;
            case RawBodyKind::Description:
              matchClose = eqIc(rawBodyTagBuf_, rawBodyTagBufLen_, "description", 11);
              break;
            case RawBodyKind::Stylesheet:
              matchClose = eqIc(rawBodyTagBuf_, rawBodyTagBufLen_, "stylesheet", 10);
              break;
            case RawBodyKind::None:
              break;
          }
          if (matchClose) {
            rawBody_ = RawBodyKind::None;
            if (b == '>') state_ = State::Text;
            else          state_ = State::RawBodyAfter;
          } else {
            // Not the closing tag we're waiting for; continue scanning.
            if (b == '<') state_ = State::RawBodyLT;
            else          state_ = State::RawBody;
          }
        }
        break;

      case State::RawBodyAfter:
        if (b == '>') state_ = State::Text;
        break;
    }
  }
  return len;  // full consumption — no early exit was taken
}

}  // namespace snapix::smolport
