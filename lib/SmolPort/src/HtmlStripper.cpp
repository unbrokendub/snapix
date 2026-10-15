#include "HtmlStripper.h"

#include "Markers.h"
#include "TextHygiene.h"

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
// Phase 3c / v3.11.12: named entity tables — the full HTML 4 set plus &apos;.
// Pre-v3.11.12 only ~60 names were known and the rest were DROPPED (&bdquo;
// &aacute; &minus; &prime; Greek, arrows …); unknown names now stay literal.
// ---------------------------------------------------------------------------
// Latin-1 block: kLatin1Names[i] is the name of U+00A0 + i.
constexpr const char* kLatin1Names[96] = {
    "nbsp",   "iexcl",  "cent",   "pound",  "curren", "yen",    "brvbar", "sect",
    "uml",    "copy",   "ordf",   "laquo",  "not",    "shy",    "reg",    "macr",
    "deg",    "plusmn", "sup2",   "sup3",   "acute",  "micro",  "para",   "middot",
    "cedil",  "sup1",   "ordm",   "raquo",  "frac14", "frac12", "frac34", "iquest",
    "Agrave", "Aacute", "Acirc",  "Atilde", "Auml",   "Aring",  "AElig",  "Ccedil",
    "Egrave", "Eacute", "Ecirc",  "Euml",   "Igrave", "Iacute", "Icirc",  "Iuml",
    "ETH",    "Ntilde", "Ograve", "Oacute", "Ocirc",  "Otilde", "Ouml",   "times",
    "Oslash", "Ugrave", "Uacute", "Ucirc",  "Uuml",   "Yacute", "THORN",  "szlig",
    "agrave", "aacute", "acirc",  "atilde", "auml",   "aring",  "aelig",  "ccedil",
    "egrave", "eacute", "ecirc",  "euml",   "igrave", "iacute", "icirc",  "iuml",
    "eth",    "ntilde", "ograve", "oacute", "ocirc",  "otilde", "ouml",   "divide",
    "oslash", "ugrave", "uacute", "ucirc",  "uuml",   "yacute", "thorn",  "yuml",
};

struct NamedEntity {
  const char* name;
  uint16_t    codepoint;
};

// Everything outside Latin-1.  Most frequent in books first (dashes, quotes,
// ellipsis) so the linear scan exits early on the common names.  The first
// kLegacyNamedEntities entries also decode without a trailing ';'.
constexpr size_t kLegacyNamedEntities = 4;
constexpr NamedEntity kNamedEntities[] = {
    {"amp", 0x0026}, {"lt", 0x003C}, {"gt", 0x003E}, {"quot", 0x0022}, {"apos", 0x0027},
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026},
    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201A},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E},
    {"lsaquo", 0x2039}, {"rsaquo", 0x203A},
    {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009},
    {"zwnj", 0x200C}, {"zwj", 0x200D}, {"lrm", 0x200E}, {"rlm", 0x200F},
    {"dagger", 0x2020}, {"Dagger", 0x2021}, {"bull", 0x2022}, {"permil", 0x2030},
    {"prime", 0x2032}, {"Prime", 0x2033}, {"oline", 0x203E}, {"frasl", 0x2044},
    {"euro", 0x20AC}, {"trade", 0x2122},
    {"OElig", 0x0152}, {"oelig", 0x0153}, {"Scaron", 0x0160}, {"scaron", 0x0161},
    {"Yuml", 0x0178}, {"fnof", 0x0192}, {"circ", 0x02C6}, {"tilde", 0x02DC},
    // Greek.
    {"Alpha", 0x0391}, {"Beta", 0x0392}, {"Gamma", 0x0393}, {"Delta", 0x0394},
    {"Epsilon", 0x0395}, {"Zeta", 0x0396}, {"Eta", 0x0397}, {"Theta", 0x0398},
    {"Iota", 0x0399}, {"Kappa", 0x039A}, {"Lambda", 0x039B}, {"Mu", 0x039C},
    {"Nu", 0x039D}, {"Xi", 0x039E}, {"Omicron", 0x039F}, {"Pi", 0x03A0},
    {"Rho", 0x03A1}, {"Sigma", 0x03A3}, {"Tau", 0x03A4}, {"Upsilon", 0x03A5},
    {"Phi", 0x03A6}, {"Chi", 0x03A7}, {"Psi", 0x03A8}, {"Omega", 0x03A9},
    {"alpha", 0x03B1}, {"beta", 0x03B2}, {"gamma", 0x03B3}, {"delta", 0x03B4},
    {"epsilon", 0x03B5}, {"zeta", 0x03B6}, {"eta", 0x03B7}, {"theta", 0x03B8},
    {"iota", 0x03B9}, {"kappa", 0x03BA}, {"lambda", 0x03BB}, {"mu", 0x03BC},
    {"nu", 0x03BD}, {"xi", 0x03BE}, {"omicron", 0x03BF}, {"pi", 0x03C0},
    {"rho", 0x03C1}, {"sigmaf", 0x03C2}, {"sigma", 0x03C3}, {"tau", 0x03C4},
    {"upsilon", 0x03C5}, {"phi", 0x03C6}, {"chi", 0x03C7}, {"psi", 0x03C8},
    {"omega", 0x03C9}, {"thetasym", 0x03D1}, {"upsih", 0x03D2}, {"piv", 0x03D6},
    // Letterlike / arrows.
    {"image", 0x2111}, {"weierp", 0x2118}, {"real", 0x211C}, {"alefsym", 0x2135},
    {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193},
    {"harr", 0x2194}, {"crarr", 0x21B5}, {"lArr", 0x21D0}, {"uArr", 0x21D1},
    {"rArr", 0x21D2}, {"dArr", 0x21D3}, {"hArr", 0x21D4},
    // Math.
    {"forall", 0x2200}, {"part", 0x2202}, {"exist", 0x2203}, {"empty", 0x2205},
    {"nabla", 0x2207}, {"isin", 0x2208}, {"notin", 0x2209}, {"ni", 0x220B},
    {"prod", 0x220F}, {"sum", 0x2211}, {"minus", 0x2212}, {"lowast", 0x2217},
    {"radic", 0x221A}, {"prop", 0x221D}, {"infin", 0x221E}, {"ang", 0x2220},
    {"and", 0x2227}, {"or", 0x2228}, {"cap", 0x2229}, {"cup", 0x222A},
    {"int", 0x222B}, {"there4", 0x2234}, {"sim", 0x223C}, {"cong", 0x2245},
    {"asymp", 0x2248}, {"ne", 0x2260}, {"equiv", 0x2261}, {"le", 0x2264},
    {"ge", 0x2265}, {"sub", 0x2282}, {"sup", 0x2283}, {"nsub", 0x2284},
    {"sube", 0x2286}, {"supe", 0x2287}, {"oplus", 0x2295}, {"otimes", 0x2297},
    {"perp", 0x22A5}, {"sdot", 0x22C5}, {"lceil", 0x2308}, {"rceil", 0x2309},
    {"lfloor", 0x230A}, {"rfloor", 0x230B}, {"lang", 0x2329}, {"rang", 0x232A},
    {"loz", 0x25CA}, {"spades", 0x2660}, {"clubs", 0x2663}, {"hearts", 0x2665},
    {"diams", 0x2666},
};

// Exact, case-sensitive match of a NUL-terminated table name.
inline bool nameEq(const char* name, const uint8_t* buf, const uint8_t len) {
  for (uint8_t i = 0; i < len; ++i) {
    if (name[i] != static_cast<char>(buf[i])) return false;
  }
  return name[len] == '\0';
}

// Entity-name byte: alnum, plus '#' as the first byte of a numeric reference.
inline bool isEntityChar(const uint8_t b, const uint8_t pos) {
  return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') ||
         (b == '#' && pos == 0);
}

// `href` or any `prefix:href` (xlink:href, l:href, …).
inline bool isHrefAttr(const uint8_t* name, const uint8_t len) {
  if (eqIc(name, len, "href", 4)) return true;
  return len > 5 && name[len - 5] == ':' && eqIc(name + len - 4, 4, "href", 4);
}

}  // namespace

// ===========================================================================
// Construction / lifecycle.
// ===========================================================================
HtmlStripper::HtmlStripper(HtmlStripperSink& sink, Mode mode, const CssStyleTable* styles)
    : gate_(sink),
      sink_((styles != nullptr && mode == Mode::Html) ? static_cast<HtmlStripperSink&>(gate_) : sink),
      state_(State::Text),
      mode_(mode),
      tagNameLen_(0),
      isEndTag_(false),
      selfClosing_(false),
      attrNameLen_(0),
      attrQuote_(0),
      captureCurrent_(false),
      entityLen_(0),
      utf8Len_(0),
      utf8Need_(0),
      payloadLen_(0),
      captureKind_(AttrKind::None),
      rawBody_(RawBodyKind::None),
      rawBodyTagBufLen_(0),
      css_(mode == Mode::Html ? styles : nullptr),
      elemDepth_(0),
      untracked_(0),
      hiddenDepth_(0),
      italicOpen_(0),
      boldOpen_(0),
      centerOpen_(0),
      cssIndentOpen_(0),
      cssAttr_(CssAttr::None),
      classInToken_(false),
      tagDotHash_(kCssHashSeed),
      classHash_(kCssHashSeed),
      tagClassHash_(kCssHashSeed) {}

void HtmlStripper::reset() {
  gate_.closed = false;
  elemDepth_ = 0;
  untracked_ = 0;
  hiddenDepth_ = 0;
  italicOpen_ = 0;
  boldOpen_ = 0;
  centerOpen_ = 0;
  cssIndentOpen_ = 0;
  cssAttr_ = CssAttr::None;
  classInToken_ = false;
  classFx_ = CssEffects{};
  tagClassFx_ = CssEffects{};
  inlineFx_ = CssEffects{};
  inlineDecl_.reset();
  state_ = State::Text;
  tagNameLen_ = 0;
  isEndTag_ = false;
  selfClosing_ = false;
  attrNameLen_ = 0;
  attrQuote_ = 0;
  captureCurrent_ = false;
  entityLen_ = 0;
  utf8Len_ = 0;
  utf8Need_ = 0;
  payloadLen_ = 0;
  captureKind_ = AttrKind::None;
  rawBody_ = RawBodyKind::None;
  rawBodyTagBufLen_ = 0;
}

void HtmlStripper::finish() {
  // Input ended mid-construct: whatever was held back as a possible entity or
  // tag start is plain text after all.
  if (state_ == State::EntityBody) {
    flushEntityLiteral();
  } else if (state_ == State::TagOpen) {
    emitChar('<');
  }
  state_ = State::Text;
  flushUtf8();
}

// ===========================================================================
// Emit helpers.
// ===========================================================================
void HtmlStripper::emitChar(const uint8_t b) {
  if (utf8Need_ > 0) {
    if ((b & 0xC0u) == 0x80u) {
      utf8Buf_[utf8Len_++] = b;
      if (--utf8Need_ == 0) {
        uint32_t cp;
        if (utf8Len_ == 2) {
          cp = ((utf8Buf_[0] & 0x1Fu) << 6) | (utf8Buf_[1] & 0x3Fu);
        } else if (utf8Len_ == 3) {
          cp = ((utf8Buf_[0] & 0x0Fu) << 12) | ((utf8Buf_[1] & 0x3Fu) << 6) | (utf8Buf_[2] & 0x3Fu);
        } else {
          cp = ((utf8Buf_[0] & 0x07u) << 18) | ((utf8Buf_[1] & 0x3Fu) << 12) |
               ((utf8Buf_[2] & 0x3Fu) << 6) | (utf8Buf_[3] & 0x3Fu);
        }
        const uint8_t n = utf8Len_;
        utf8Len_ = 0;
        emitCodepoint(cp, utf8Buf_, n);
      }
      return;
    }
    flushUtf8();  // truncated sequence — pass it through, then handle `b`
  }
  if (b >= 0xC2u && b <= 0xF4u) {
    utf8Buf_[0] = b;
    utf8Len_ = 1;
    utf8Need_ = b >= 0xF0u ? 3 : (b >= 0xE0u ? 2 : 1);
    return;
  }
  if (b == kMarker) {
    const uint8_t pair[2] = {kMarker, kMarker};
    sink_.emit(pair, 2);
    return;
  }
  if (b < 0x20u || b == 0x7Fu) {
    // C0 controls have no glyph.  Keep the whitespace ones; VT/FF are spaces.
    if (b == '\t' || b == '\n' || b == '\r') {
      sink_.emit(&b, 1);
    } else if (b == 0x0Bu || b == 0x0Cu) {
      const uint8_t sp = ' ';
      sink_.emit(&sp, 1);
    }
    return;
  }
  sink_.emit(&b, 1);
}

// Final per-code-point gate for multi-byte text (raw UTF-8 or an entity).
void HtmlStripper::emitCodepoint(const uint32_t cp, const uint8_t* bytes, const uint8_t n) {
  const uint32_t mapped = mapTextCodepoint(cp);  // TextHygiene.h
  if (mapped == cp) {
    sink_.emit(bytes, n);
  } else if (mapped != 0) {
    emitUtf8(mapped);
  }
}

void HtmlStripper::flushUtf8() {
  if (utf8Len_ == 0) return;
  sink_.emit(utf8Buf_, utf8Len_);
  utf8Len_ = 0;
  utf8Need_ = 0;
}

void HtmlStripper::emitRaw(const uint8_t* data, const size_t len) {
  flushUtf8();
  sink_.emit(data, len);
}

void HtmlStripper::emitMarker(const uint8_t tag) {
  const uint8_t bytes[2] = {kMarker, tag};
  emitRaw(bytes, 2);
}

void HtmlStripper::emitPayloadMarker(const uint8_t tag) {
  const uint8_t hdr[4] = {kMarker, tag,
                           static_cast<uint8_t>(payloadLen_ & 0xFFu),
                           static_cast<uint8_t>((payloadLen_ >> 8) & 0xFFu)};
  emitRaw(hdr, 4);
  if (payloadLen_ > 0) sink_.emit(payload_, payloadLen_);
}

void HtmlStripper::emitUtf8(const uint32_t cp) {
  if (cp < 0x80u) {
    emitChar(static_cast<uint8_t>(cp));
    return;
  }
  flushUtf8();
  uint8_t buf[4];
  uint8_t n;
  if (cp < 0x800u) {
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
  emitCodepoint(cp, buf, n);
}

void HtmlStripper::flushEntityLiteral() {
  emitChar('&');
  for (uint8_t i = 0; i < entityLen_; ++i) emitChar(entityBuf_[i]);
  entityLen_ = 0;
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
        emitRaw(bytes, 3);
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
        emitRaw(bytes, 3);
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
    //
    // v3.10.7 — FALL THROUGH to the common dispatch below for tags FB2 shares
    // with HTML (sup/sub, b/i, h1-h6, table/tr/td, br/hr, …).  Pre-fix a blanket
    // `return` here swallowed them, so FB2 <sup>/<sub> rendered as plain text and
    // FB2 <table> cells ran together — even though the sup/sub comment below
    // claimed FB2 fell through.  Every FB2-specific tag above already returned,
    // so only the shared/unknown tags reach the common table.
  }

  // ----- HTML dispatch table (existing Phase 3b) -----
  if (eqIc(tagName_, tagNameLen_, "b", 1) ||
      eqIc(tagName_, tagNameLen_, "strong", 6)) {
    emitStyle(isEndTag_ ? kBoldOff : kBoldOn);
    return;
  }
  if (eqIc(tagName_, tagNameLen_, "i", 1) ||
      eqIc(tagName_, tagNameLen_, "em", 2)) {
    emitStyle(isEndTag_ ? kItalicOff : kItalicOn);
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
      emitStyle(kCenterOff);
      emitMarker(kParagraphBreak);
    } else {
      emitMarker(kParagraphBreak);
      emitStyle(kCenterOn);
      const uint8_t bytes[3] = {kMarker, kHeadingOn, tagName_[1]};
      emitRaw(bytes, 3);
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
      emitStyle(kCenterOff);
      emitMarker(kParagraphBreak);
    } else {
      emitMarker(kParagraphBreak);
      emitStyle(kCenterOn);
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
      emitRaw(kBulletBytes, sizeof(kBulletBytes));
    }
    return;
  }
  // v3.10.7 — HTML5 semantic block containers + <figure>/<figcaption>.  These
  // are block-level like <div>; without a break their content collapses into the
  // surrounding text (same class as the pre-v3.10.6 <h1>-<h6> bug — modern EPUBs
  // wrap chapters/scene-blocks in <section>/<article>/<header> etc.).  Treat like
  // <div>: paragraph break on CLOSE (open break would double-break a wrapped <p>).
  if (eqIc(tagName_, tagNameLen_, "section", 7) ||
      eqIc(tagName_, tagNameLen_, "article", 7) ||
      eqIc(tagName_, tagNameLen_, "header", 6) ||
      eqIc(tagName_, tagNameLen_, "footer", 6) ||
      eqIc(tagName_, tagNameLen_, "aside", 5) ||
      eqIc(tagName_, tagNameLen_, "figure", 6) ||
      eqIc(tagName_, tagNameLen_, "figcaption", 10) ||
      eqIc(tagName_, tagNameLen_, "nav", 3) ||
      eqIc(tagName_, tagNameLen_, "main", 4)) {
    if (isEndTag_) emitMarker(kParagraphBreak);
    return;
  }
  // v3.10.7 — tables.  No real column layout on a 1-bit reader, but emit
  // row/cell breaks so cells don't concatenate into one run.  <table>/<caption>
  // isolate the block (paragraph break both sides); each <tr> starts a new line;
  // each cell close emits a 2-space gap so adjacent cells read as separate words.
  // <thead>/<tbody>/<tfoot> are structural only — rows handle the layout.
  if (eqIc(tagName_, tagNameLen_, "table", 5) ||
      eqIc(tagName_, tagNameLen_, "caption", 7)) {
    emitMarker(kParagraphBreak);
    return;
  }
  if (eqIc(tagName_, tagNameLen_, "tr", 2)) {
    if (!isEndTag_) emitMarker(kLineBreak);
    return;
  }
  if (eqIc(tagName_, tagNameLen_, "td", 2) || eqIc(tagName_, tagNameLen_, "th", 2)) {
    if (isEndTag_) {
      static const uint8_t kCellGap[] = {' ', ' '};
      emitRaw(kCellGap, sizeof(kCellGap));
    }
    return;
  }
  // v3.10.7 — definition lists.  <dl> isolates the block; <dt> term on its own
  // line; <dd> description on its own line, indented (matches the usual visual).
  if (eqIc(tagName_, tagNameLen_, "dl", 2)) {
    emitMarker(kParagraphBreak);
    return;
  }
  if (eqIc(tagName_, tagNameLen_, "dt", 2)) {
    if (!isEndTag_) emitMarker(kLineBreak);
    return;
  }
  if (eqIc(tagName_, tagNameLen_, "dd", 2)) {
    if (isEndTag_) {
      emitMarker(kIndentOff);
    } else {
      emitMarker(kLineBreak);
      emitMarker(kIndentOn);
    }
    return;
  }
  // v3.10.7 — HTML <cite> is conventionally italic (a referenced title); the FB2
  // dispatch above maps its own <cite> to a block quote, so this only applies to
  // HTML.  Other inline tags we deliberately leave unstyled (text still shown):
  // <u>/<s>/<strike>/<del>/<code>/<tt>/<kbd>/<samp>/<span>/<mark>/<small>/<q> —
  // no underline/strike/mono support, but the words render normally.
  if (eqIc(tagName_, tagNameLen_, "cite", 4)) {
    emitStyle(isEndTag_ ? kItalicOff : kItalicOn);
    return;
  }
  // Unknown / unhandled tag — text content still emitted by outer loop.
}

// ===========================================================================
// Entity dispatch (Phase 3c).  Returns false when the buffered name is not an
// entity we can decode — the caller then emits it literally.  `terminated` is
// false when no ';' followed: then only numeric references and the legacy
// names browsers accept without ';' (Latin-1 + amp/lt/gt/quot) decode, so a
// bare "this&or that" stays text instead of becoming "this∨ that".
// ===========================================================================
bool HtmlStripper::dispatchEntity(const bool terminated) {
  if (entityLen_ == 0) return false;

  if (entityBuf_[0] == '#') {
    if (entityLen_ < 2) return false;
    uint32_t cp = 0;
    bool overflow = false;
    if (entityBuf_[1] == 'x' || entityBuf_[1] == 'X') {
      if (entityLen_ < 3) return false;
      for (uint8_t i = 2; i < entityLen_; ++i) {
        const uint8_t c = entityBuf_[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        if (cp > (0x10FFFFu >> 4)) overflow = true;
        else cp = (cp << 4) | d;
      }
    } else {
      for (uint8_t i = 1; i < entityLen_; ++i) {
        const uint8_t c = entityBuf_[i];
        if (c < '0' || c > '9') return false;
        if (cp > (0x10FFFFu / 10)) overflow = true;
        else cp = cp * 10 + static_cast<uint32_t>(c - '0');
      }
    }
    // Out-of-range / NUL / surrogate → U+FFFD, as browsers do.
    if (overflow || cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;
    emitUtf8(cp);
    return true;
  }

  for (uint8_t i = 0; i < 96; ++i) {
    if (nameEq(kLatin1Names[i], entityBuf_, entityLen_)) {
      emitUtf8(0x00A0u + i);
      return true;
    }
  }
  const size_t searchable = terminated ? sizeof(kNamedEntities) / sizeof(kNamedEntities[0])
                                       : kLegacyNamedEntities;
  for (size_t i = 0; i < searchable; ++i) {
    if (nameEq(kNamedEntities[i].name, entityBuf_, entityLen_)) {
      emitUtf8(kNamedEntities[i].codepoint);
      return true;
    }
  }
  return false;
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
    if (captureKind_ == AttrKind::Id) {
      // v3.11.12 — an anchor survives display:none: page-list and footnote
      // targets are often hidden page-number spans.  It has no glyphs.
      const bool closed = gate_.closed;
      flushUtf8();  // a held-back partial sequence belongs to the hidden text
      gate_.closed = false;
      emitPayloadMarker(kAnchor);
      gate_.closed = closed;
    } else if (captureKind_ == AttrKind::Src) {
      emitPayloadMarker(kImageRef);
    }
  }
  if (cssAttr_ == CssAttr::Class) {
    endClassToken();
  } else if (cssAttr_ == CssAttr::Style) {
    inlineFx_.apply(inlineDecl_.finish());
  }
  cssAttr_ = CssAttr::None;
  captureCurrent_ = false;
  captureKind_ = AttrKind::None;
  attrNameLen_ = 0;
}

// Tags whose content is never rendered.  `id` on them is not an anchor.
bool HtmlStripper::isRawBodyTag() const {
  if (mode_ == Mode::Fb2) {
    return eqIc(tagName_, tagNameLen_, "binary", 6) ||
           eqIc(tagName_, tagNameLen_, "description", 11) ||
           eqIc(tagName_, tagNameLen_, "stylesheet", 10);
  }
  return eqIc(tagName_, tagNameLen_, "script", 6) ||
         eqIc(tagName_, tagNameLen_, "style", 5) ||
         eqIc(tagName_, tagNameLen_, "title", 5) ||
         eqIc(tagName_, tagNameLen_, "desc", 4) ||
         eqIc(tagName_, tagNameLen_, "metadata", 8);
}

// An attribute value starts: decide whether (and as what) it is captured.
//   `id` on any non-raw-body open tag            → kAnchor
//   HTML `<img src>`                             → kImageRef
//   `<image href>` / `<image xlink:href>` (SVG in HTML; FB2 `l:href` or any
//   other namespace prefix)                       → kImageRef
void HtmlStripper::beginAttrValue() {
  captureKind_ = AttrKind::None;
  if (!isEndTag_) {
    if (eqIc(attrName_, attrNameLen_, "id", 2)) {
      if (!isRawBodyTag()) captureKind_ = AttrKind::Id;
    } else if (mode_ == Mode::Html && eqIc(tagName_, tagNameLen_, "img", 3)) {
      if (eqIc(attrName_, attrNameLen_, "src", 3)) captureKind_ = AttrKind::Src;
    } else if (eqIc(tagName_, tagNameLen_, "image", 5) ||
               (mode_ == Mode::Html && eqIc(tagName_, tagNameLen_, "svg:image", 9))) {
      if (isHrefAttr(attrName_, attrNameLen_)) captureKind_ = AttrKind::Src;
    }
  }
  captureCurrent_ = (captureKind_ != AttrKind::None);
  payloadLen_ = 0;
  // v3.11.12 — `class` / `style` are resolved byte by byte (never buffered).
  cssAttr_ = CssAttr::None;
  if (css_ != nullptr && !isEndTag_ && !captureCurrent_) {
    if (eqIc(attrName_, attrNameLen_, "class", 5)) {
      cssAttr_ = CssAttr::Class;
      classInToken_ = false;
      tagDotHash_ = cssHash(cssHashLower(kCssHashSeed, tagName_, tagNameLen_), '.');
    } else if (eqIc(attrName_, attrNameLen_, "style", 5)) {
      cssAttr_ = CssAttr::Style;
      inlineDecl_.reset();
    }
  }
}

// ===========================================================================
// v3.11.12 — CSS resolution (Html mode with a style table).
// ===========================================================================
// Italic / bold / centre can have several nested sources (tags and CSS).  With
// CSS on, the marker goes out when the first source opens and the last one
// closes; a close with nothing open still emits (harmless, and keeps a stray
// `</i>` behaving as before).  Without CSS the marker is emitted as is.
void HtmlStripper::emitStyle(const uint8_t tag) {
  uint8_t* open = nullptr;
  if (css_ != nullptr) {
    if (tag == kItalicOn || tag == kItalicOff) open = &italicOpen_;
    else if (tag == kBoldOn || tag == kBoldOff) open = &boldOpen_;
    else if (tag == kCenterOn || tag == kCenterOff) open = &centerOpen_;
  }
  if (open == nullptr) {
    emitMarker(tag);
    return;
  }
  if (tag == kItalicOn || tag == kBoldOn || tag == kCenterOn) {
    if (*open == 0) emitMarker(tag);
    if (*open < 0xFF) ++*open;
  } else if (*open <= 1) {
    *open = 0;
    emitMarker(tag);
  } else {
    --*open;
  }
}

void HtmlStripper::cssAttrByte(const uint8_t b) {
  if (cssAttr_ == CssAttr::Style) {
    inlineDecl_.feed(b);
    return;
  }
  if (isWhitespace(b)) {
    endClassToken();
    return;
  }
  if (!classInToken_) {
    classInToken_ = true;
    classHash_ = cssHash(kCssHashSeed, '.');
    tagClassHash_ = tagDotHash_;
  }
  classHash_ = cssHash(classHash_, b);  // class names are case-sensitive
  tagClassHash_ = cssHash(tagClassHash_, b);
}

void HtmlStripper::endClassToken() {
  if (!classInToken_) return;
  classInToken_ = false;
  classFx_.apply(css_->lookup(classHash_));
  tagClassFx_.apply(css_->lookup(tagClassHash_));
}

// HTML void elements never have a close tag (`<br>`, `<img …>`); pushing them
// would leave stale frames.  SVG <image> is parsed as <img> by HTML5.
bool HtmlStripper::isVoidTag() const {
  static constexpr const char* kVoid[] = {"br",   "img",   "hr",     "meta",  "link", "input", "col",
                                          "area", "base",  "embed",  "param", "source", "track", "wbr",
                                          "image"};
  for (const char* v : kVoid) {
    if (eqIc(tagName_, tagNameLen_, v, std::strlen(v))) return true;
  }
  return false;
}

// Block containers that may take text-align:center / a left-margin indent
// from CSS.  Inline elements (span, a, …) ignore both, like browsers; list
// items and table cells are left alone so bullets and rows keep their layout.
bool HtmlStripper::isCssBlockTag() const {
  if (tagNameLen_ == 2 && (tagName_[0] | 0x20u) == 'h' && tagName_[1] >= '1' && tagName_[1] <= '6') return true;
  static constexpr const char* kBlocks[] = {"p",     "div",    "blockquote", "center", "section", "article",
                                            "header", "footer", "aside",      "figure", "figcaption", "main",
                                            "nav",   "address", "pre"};
  for (const char* t : kBlocks) {
    if (eqIc(tagName_, tagNameLen_, t, std::strlen(t))) return true;
  }
  return false;
}

void HtmlStripper::setHidden(const bool hidden) {
  flushUtf8();  // a held-back partial sequence belongs to the text before the switch
  gate_.closed = hidden;
}

void HtmlStripper::pushElement(const uint32_t hash, const uint8_t fx, const uint16_t saved) {
  elemHash_[elemDepth_] = hash;
  elemFx_[elemDepth_] = fx;
  elemSaved_[elemDepth_] = saved;
  ++elemDepth_;
}

namespace {
// elemSaved_ packing: 5 bits per suspended style.
constexpr uint8_t kSavedItalicShift = 0;
constexpr uint8_t kSavedBoldShift = 5;
constexpr uint8_t kSavedCenterShift = 10;
}  // namespace

// Close the open elements above `depth`, innermost first, undoing the CSS
// effects each one switched on and resuming the styles it suspended.  Returns
// true when the element at `depth` (the one being closed) was hidden — itself
// display:none or inside such an element — so its own close markers must not
// be emitted.
bool HtmlStripper::popElements(const uint8_t depth) {
  bool closedHidden = false;
  while (elemDepth_ > depth) {
    const uint8_t level = elemDepth_;  // 1-based depth of this frame
    --elemDepth_;
    if (hiddenDepth_ != 0) {
      if (elemDepth_ == depth) closedHidden = true;
      if (hiddenDepth_ == level) {
        hiddenDepth_ = 0;
        setHidden(false);
      }
      continue;  // frames inside a hidden subtree switched nothing on
    }
    const uint8_t fx = elemFx_[elemDepth_];
    const uint16_t saved = elemSaved_[elemDepth_];
    // Resume a suspended style: its outer sources are open again (plus any
    // source left unclosed inside this element).
    auto resume = [this, saved](const uint8_t shift, uint8_t& open, const uint8_t onTag) {
      const uint8_t outer = static_cast<uint8_t>((saved >> shift) & 0x1Fu);
      if (outer == 0) return;
      if (open == 0) emitMarker(onTag);
      open = static_cast<uint8_t>(open + outer < 0xFF ? open + outer : 0xFF);
    };
    if (fx & kCssItalic) emitStyle(kItalicOff);
    resume(kSavedItalicShift, italicOpen_, kItalicOn);
    if (fx & kCssBold) emitStyle(kBoldOff);
    resume(kSavedBoldShift, boldOpen_, kBoldOn);
    if (fx & kCssCenter) emitStyle(kCenterOff);
    resume(kSavedCenterShift, centerOpen_, kCenterOn);
    if (fx & kCssIndent) {
      emitMarker(kIndentOff);
      if (cssIndentOpen_ > 0) --cssIndentOpen_;
    }
  }
  return closedHidden;
}

// onTagComplete's dispatch when CSS is on.  Open tag: the tag's own markers,
// then its CSS effects (pushed so the matching close undoes them, innermost
// first, before the tag's close markers).  Inside a display:none subtree
// nothing is dispatched, but elements are still pushed / popped so the
// subtree ends at the right close tag.
void HtmlStripper::cssTag() {
  if (tagNameLen_ == 0) return;  // <?xml …?>, <!DOCTYPE …>
  const uint32_t hash = cssHashLower(kCssHashSeed, tagName_, tagNameLen_);

  if (isEndTag_) {
    if (isVoidTag()) {  // `</br>` — never pushed
      if (hiddenDepth_ == 0) dispatchTag();
      return;
    }
    if (untracked_ > 0) {  // closes the innermost untracked element
      --untracked_;
      if (hiddenDepth_ == 0) dispatchTag();
      return;
    }
    int k = static_cast<int>(elemDepth_) - 1;
    while (k >= 0 && elemHash_[k] != hash) --k;
    if (k < 0) {  // stray close: no element to end
      if (hiddenDepth_ == 0) dispatchTag();
      return;
    }
    if (!popElements(static_cast<uint8_t>(k))) dispatchTag();
    return;
  }

  // Open tag.  Cascade: tag < .class < tag.class < inline style.
  CssEffects fx = css_->lookup(hash);
  fx.apply(classFx_);
  fx.apply(tagClassFx_);
  fx.apply(inlineFx_);

  if (selfClosing_ || isVoidTag()) {  // never on the stack
    const bool show = hiddenDepth_ == 0 && !fx.on(kCssHidden);
    if (show) dispatchTag();
    if (selfClosing_) {
      isEndTag_ = true;  // onTagComplete then returns without a raw body
      if (show) dispatchTag();
    }
    return;
  }
  if (isRawBodyTag()) {  // <script>/<style>/<title>…: body skipped by the raw-body scanner
    if (hiddenDepth_ == 0) dispatchTag();
    return;
  }
  // HTML: a <p> / <li> start tag closes an open one (sloppy unclosed markup).
  if (elemDepth_ > 0 && untracked_ == 0 && elemHash_[elemDepth_ - 1] == hash &&
      (eqIc(tagName_, tagNameLen_, "p", 1) || eqIc(tagName_, tagNameLen_, "li", 2))) {
    if (!popElements(static_cast<uint8_t>(elemDepth_ - 1))) {
      isEndTag_ = true;
      dispatchTag();
      isEndTag_ = false;
    }
  }
  if (untracked_ > 0 || elemDepth_ >= kMaxElemDepth) {
    // Too deep to track: no CSS (text always shown), tag markers as before.
    if (untracked_ < 0xFFFF) ++untracked_;
    if (hiddenDepth_ == 0) dispatchTag();
    return;
  }
  if (hiddenDepth_ != 0) {
    pushElement(hash, 0);
    return;
  }
  if (fx.on(kCssHidden)) {
    pushElement(hash, kCssHidden);
    hiddenDepth_ = elemDepth_;
    setHidden(true);
    return;
  }
  dispatchTag();
  uint8_t applied = 0;
  uint16_t saved = 0;
  // A counted style the element's CSS declares: on → one more source; off
  // while on → suspended (count saved, resumed at the close).  This also lets
  // CSS override the tag itself (`<i class="roman">`, `<h2 style=
  // "text-align:left">`), as author CSS does in a browser.
  auto counted = [&](const uint8_t bit, uint8_t& open, const uint8_t onTag, const uint8_t offTag,
                     const uint8_t shift) {
    if ((fx.mask & bit) == 0) return;
    if (fx.on(bit)) {
      emitStyle(onTag);
      applied = static_cast<uint8_t>(applied | bit);
    } else if (open > 0) {
      saved = static_cast<uint16_t>(saved | (static_cast<uint16_t>(open < 0x1F ? open : 0x1F) << shift));
      open = 0;
      emitMarker(offTag);
    }
  };
  if (isCssBlockTag()) {
    // One CSS indent level at a time: nested margins do not compound, and a
    // tag that already indents (blockquote) is not indented twice.
    if (fx.on(kCssIndent) && cssIndentOpen_ == 0 && !eqIc(tagName_, tagNameLen_, "blockquote", 10)) {
      emitMarker(kIndentOn);
      ++cssIndentOpen_;
      applied |= kCssIndent;
    }
    counted(kCssCenter, centerOpen_, kCenterOn, kCenterOff, kSavedCenterShift);
  }
  counted(kCssBold, boldOpen_, kBoldOn, kBoldOff, kSavedBoldShift);
  counted(kCssItalic, italicOpen_, kItalicOn, kItalicOff, kSavedItalicShift);
  pushElement(hash, applied, saved);
}

// At tag close we still need to:
//   - emit style markers (kBoldOn/Off etc.)
//   - enter raw-body suppression for <script>/<style>/<title>/…
// Payload markers (img/a) were already emitted by onAttrValueComplete.
void HtmlStripper::onTagComplete() {
  if (css_ != nullptr) {
    cssTag();  // also dispatches a self-closing tag's close (and sets isEndTag_)
  } else {
    dispatchTag();
  }
  state_ = State::Text;
  if (isEndTag_) return;
  if (selfClosing_) {
    // v3.11.12 — `<p/>`, `<b/>`, `<h2/>`, FB2 `<title/>` … are open+close.
    // Pre-fix only the OPEN was dispatched, so `<b/>` left bold on (and
    // `<h2/>` a centred heading) for the rest of the chapter, and
    // `<script src="x.js"/>` entered a raw body that swallowed every byte up
    // to the next `</script>` — or the whole remaining chapter.
    isEndTag_ = true;
    dispatchTag();
    return;
  }
  if (mode_ == Mode::Fb2) {
    // FB2 raw bodies: <binary> holds base64 image data, <description>
    // holds book metadata (title-info / publish-info), <stylesheet>
    // holds CSS.  None contribute to renderable text.
    if (eqIc(tagName_, tagNameLen_, "binary", 6)) {
      rawBody_ = RawBodyKind::Binary;
    } else if (eqIc(tagName_, tagNameLen_, "description", 11)) {
      rawBody_ = RawBodyKind::Description;
    } else if (eqIc(tagName_, tagNameLen_, "stylesheet", 10)) {
      rawBody_ = RawBodyKind::Stylesheet;
    }
  } else {
    // HTML raw bodies.  v3.11.12 — <title> (document title in <head>, and
    // SVG <title>/<desc>/<metadata> tooltips/RDF) was rendered as the first
    // line of every chapter, glued to the body text.
    if (eqIc(tagName_, tagNameLen_, "script", 6)) {
      rawBody_ = RawBodyKind::Script;
    } else if (eqIc(tagName_, tagNameLen_, "style", 5)) {
      rawBody_ = RawBodyKind::Style;
    } else if (eqIc(tagName_, tagNameLen_, "title", 5)) {
      rawBody_ = RawBodyKind::Title;
    } else if (eqIc(tagName_, tagNameLen_, "desc", 4)) {
      rawBody_ = RawBodyKind::SvgDesc;
    } else if (eqIc(tagName_, tagNameLen_, "metadata", 8)) {
      rawBody_ = RawBodyKind::SvgMetadata;
    }
  }
  if (rawBody_ != RawBodyKind::None) state_ = State::RawBody;
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
    // v3.11.12 — a state that discovers its held-back prefix was literal text
    // (bare '&', stray '<') emits the prefix, switches to Text and sets
    // `again` so the SAME byte is handled there (it may start a tag/entity).
    bool again;
    do {
      again = false;
      switch (state_) {
        // ----- text -----
        case State::Text:
          if (b == '<') {
            state_ = State::TagOpen;
            tagNameLen_ = 0;
            isEndTag_ = false;
            selfClosing_ = false;
            // Clean slate: per-attr capture state will be recomputed when
            // each attribute's value starts.
            captureCurrent_ = false;
            captureKind_ = AttrKind::None;
            payloadLen_ = 0;
            cssAttr_ = CssAttr::None;
            classFx_ = CssEffects{};
            tagClassFx_ = CssEffects{};
            inlineFx_ = CssEffects{};
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
          } else if (b == '?') {
            state_ = State::AttrPreName;  // <?xml …?> — skip to '>'
          } else if (b == '/') {
            isEndTag_ = true;
            state_ = State::TagName;
          } else if (isAttrNameStart(b)) {
            tagName_[tagNameLen_++] = b;
            state_ = State::TagName;
          } else {
            // v3.11.12 — "5 < 10", "x<5", "<3": not a tag.  Pre-fix the
            // text up to the next '>' (often the rest of the paragraph)
            // was skipped as a bogus tag.
            emitChar('<');
            state_ = State::Text;
            again = true;
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
          if (isEndTag_ && tagNameLen_ == 0 && !isAttrNameStart(b)) {
            // "</" not followed by a name: "</>" is ignored, anything else
            // ("a </ b") is literal text.
            if (b == '>') {
              state_ = State::Text;
            } else {
              emitChar('<');
              emitChar('/');
              state_ = State::Text;
              again = true;
            }
          } else if (isTagNameChar(b)) {
            if (tagNameLen_ < kTagBufSize) tagName_[tagNameLen_++] = b;
          } else if (b == '>') {
            onTagComplete();
          } else {
            selfClosing_ = (b == '/');
            state_ = State::AttrPreName;
          }
          break;

        // ----- attribute scanning -----
        case State::AttrPreName:
          if (b == '>') {
            onTagComplete();
          } else if (b == '/') {
            selfClosing_ = true;  // only counts if '>' follows directly
          } else if (isAttrNameStart(b)) {
            selfClosing_ = false;
            attrNameLen_ = 0;
            attrName_[attrNameLen_++] = b;
            state_ = State::AttrName;
          } else if (!isWhitespace(b)) {
            selfClosing_ = false;
          }
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
            // Whitespace or '/': end of name, no value.  Treat as boolean.
            attrNameLen_ = 0;
            selfClosing_ = (b == '/');
            state_ = State::AttrPreName;
          }
          break;

        case State::AttrPreValue:
          if (b == '"' || b == '\'') {
            attrQuote_ = b;
            beginAttrValue();
            state_ = State::AttrValueQuoted;
          } else if (b == '>') {
            attrNameLen_ = 0;
            onTagComplete();
          } else if (isWhitespace(b)) {
            // Skip; expect value next.
          } else {
            attrQuote_ = 0;
            beginAttrValue();
            if (captureCurrent_) payload_[payloadLen_++] = b;
            else if (cssAttr_ != CssAttr::None) cssAttrByte(b);
            state_ = State::AttrValueUnq;
          }
          break;

        case State::AttrValueQuoted:
          if (b == attrQuote_) {
            onAttrValueComplete();
            state_ = State::AttrPreName;
          } else if (captureCurrent_ && payloadLen_ < kPayloadBufSize) {
            payload_[payloadLen_++] = b;
          } else if (cssAttr_ != CssAttr::None) {
            cssAttrByte(b);
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
          } else if (cssAttr_ != CssAttr::None) {
            cssAttrByte(b);
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
        // v3.11.12 — only [#]alnum+ followed by ';' is an entity.  Anything
        // else ("Tom & Jerry", "AT&T", "?a=1&b=2", an unknown "&foo;") is
        // literal text.  Pre-fix every byte up to the next ';' or '<' was
        // swallowed — with a bare '&' that was usually the rest of the
        // paragraph.  A legacy name terminated without ';' ("&nbsp ") is
        // still decoded, like browsers do.
        case State::EntityBody:
          if (b == ';') {
            if (dispatchEntity(true)) {
              entityLen_ = 0;
            } else {
              flushEntityLiteral();
              emitChar(';');
            }
            state_ = State::Text;
          } else if (isEntityChar(b, entityLen_) && entityLen_ < kEntityBufSize) {
            entityBuf_[entityLen_++] = b;
          } else {
            if (dispatchEntity(false)) entityLen_ = 0;
            else                       flushEntityLiteral();
            state_ = State::Text;
            again = true;
          }
          break;

        // ----- raw body (script / style / title …) -----
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
              // An unclosed <title>/<desc>/<metadata> (malformed source) must
              // not swallow the chapter: the enclosing </head> / </svg> also
              // ends the suppressed body.
              case RawBodyKind::Title:
                matchClose = eqIc(rawBodyTagBuf_, rawBodyTagBufLen_, "title", 5) ||
                             eqIc(rawBodyTagBuf_, rawBodyTagBufLen_, "head", 4);
                break;
              case RawBodyKind::SvgDesc:
                matchClose = eqIc(rawBodyTagBuf_, rawBodyTagBufLen_, "desc", 4) ||
                             eqIc(rawBodyTagBuf_, rawBodyTagBufLen_, "svg", 3);
                break;
              case RawBodyKind::SvgMetadata:
                matchClose = eqIc(rawBodyTagBuf_, rawBodyTagBufLen_, "metadata", 8) ||
                             eqIc(rawBodyTagBuf_, rawBodyTagBufLen_, "svg", 3);
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
    } while (again);
  }
  return len;  // full consumption — no early exit was taken
}

}  // namespace snapix::smolport
