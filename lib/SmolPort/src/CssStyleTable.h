#pragma once

// =============================================================================
// SmolPort CSS styles — the small slice of an EPUB's CSS the marker pipeline
// honours (v3.11.12).
//
// InDesign / Calibre / Sigil EPUBs mark emphasis with classes and inline styles
// instead of <i>/<b>/<center>:
//   <span class="italic">   <p class="center">   <span style="font-weight:bold">
//   <div class="pagenum" style="display:none">12</div>
// HtmlStripper resolves these effects per element (tag < .class < tag.class <
// inline style) and emits the existing markers:
//
//   font-style: italic | oblique            → italic        (kItalicOn/Off)
//   font-weight: bold | bolder | 600..900   → bold          (kBoldOn/Off)
//   font: … italic … bold …                 → both (shorthand)
//   text-align: center                      → centred block (kCenterOn/Off)
//   margin-left / margin left ≥ ~1.5em      → indented block (kIndentOn/Off)
//   display: none                           → content suppressed
//
// Selectors: `tag`, `.class`, `tag.class`, comma lists.  A selector with a
// combinator, pseudo-class, attribute, id, `*` or a second class is skipped —
// never mis-applied.  @media / @font-face / @import … are skipped whole.
//
// The book's stylesheets are compiled ONCE (CssStyleSheetCompiler, EPUB side)
// into a sorted table of 32-bit selector hashes, 6 bytes per entry, stored in
// the book's UnifiedCache and handed to the stripper (CssStyleTable).  The
// stripper itself only needs this header: hashing, the inline-style
// declaration parser and the table lookup are inline, so it stays heap-free.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace snapix::smolport {

// Effect bits.
constexpr uint8_t kCssItalic = 0x01;
constexpr uint8_t kCssBold = 0x02;
constexpr uint8_t kCssCenter = 0x04;
constexpr uint8_t kCssIndent = 0x08;
constexpr uint8_t kCssHidden = 0x10;

// The declarations of one rule (or one element's cascade): `mask` = properties
// declared, `value` = which of those are on.  value ⊆ mask.
struct CssEffects {
  uint8_t mask = 0;
  uint8_t value = 0;

  // `o` wins for every property it declares (later / more specific).
  void apply(const CssEffects& o) {
    value = static_cast<uint8_t>((value & ~o.mask) | (o.value & o.mask));
    mask = static_cast<uint8_t>(mask | o.mask);
  }
  bool on(const uint8_t bit) const { return (value & bit) != 0; }
};

// Selector keys are FNV-1a over the canonical selector text: tag names
// lower-cased, class names verbatim — "p", ".italic", "p.center".
constexpr uint32_t kCssHashSeed = 2166136261u;
inline uint32_t cssHash(const uint32_t h, const uint8_t b) { return (h ^ b) * 16777619u; }
inline uint8_t cssLower(const uint8_t b) {
  return (b >= 'A' && b <= 'Z') ? static_cast<uint8_t>(b | 0x20u) : b;
}
inline uint32_t cssHashLower(uint32_t h, const uint8_t* s, const size_t n) {
  for (size_t i = 0; i < n; ++i) h = cssHash(h, cssLower(s[i]));
  return h;
}

// -----------------------------------------------------------------------------
// CssDeclParser — streaming parser for a declaration list
// ("font-weight: bold; text-align:center").  Used for inline `style`
// attributes (fed byte by byte by the stripper) and for rule bodies.  ~40 B of
// state, no heap.  Unknown properties, functions, strings, comments and
// `!important` are tolerated and ignored.
// -----------------------------------------------------------------------------
class CssDeclParser {
 public:
  CssDeclParser() { reset(); }

  void reset() {
    fx_ = CssEffects{};
    beginDecl();
    quote_ = 0;
    parenDepth_ = 0;
    slash_ = false;
    comment_ = 0;
  }

  void feed(const uint8_t b) {
    if (comment_ != 0) {  // inside /* … */ (1) or saw its closing '*' (2)
      if (comment_ == 2 && b == '/') comment_ = 0;
      else comment_ = (b == '*') ? 2 : 1;
      return;
    }
    if (slash_) {
      slash_ = false;
      if (b == '*' && quote_ == 0) {
        comment_ = 1;
        return;
      }
      separator();  // a lone '/' (`font: 12px/1.5 serif`)
    }
    if (quote_ != 0) {  // string: content ignored, `;` inside does not end it
      if (b == quote_) quote_ = 0;
      return;
    }
    if (parenDepth_ > 0) {  // url(…), calc(…), var(…)
      if (b == '(') ++parenDepth_;
      else if (b == ')') --parenDepth_;
      return;
    }
    if (b == '/') {
      slash_ = true;
      return;
    }
    if (b == ';') {
      endDecl();
      return;
    }
    if (inValue_) {
      if (b == '"' || b == '\'') {
        tokBad_ = true;
        quote_ = b;
      } else if (b == '(') {
        tokBad_ = true;
        parenDepth_ = 1;
      } else if (b == '!') {
        separator();
        important_ = true;  // `!important` and anything after it is ignored
      } else if (b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == '\f' || b == ',') {
        separator();
      } else if (!important_) {
        if (tokLen_ < sizeof(tok_)) tok_[tokLen_++] = cssLower(b);
        else tokBad_ = true;
      }
      return;
    }
    // Property name.
    if (b == ':') {
      prop_ = lookupProp();
      inValue_ = true;
    } else if (b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == '\f') {
      if (nameLen_ > 0) nameDone_ = true;
    } else if (nameDone_ || nameLen_ >= sizeof(name_)) {
      nameBad_ = true;
    } else {
      name_[nameLen_++] = cssLower(b);
    }
  }

  // End of input: commit the last declaration; returns every declaration's
  // effects (later ones win) and resets the parser.
  CssEffects finish() {
    slash_ = false;
    endDecl();
    const CssEffects fx = fx_;
    reset();
    return fx;
  }

 private:
  enum class Prop : uint8_t { None, FontStyle, FontWeight, Font, TextAlign, Display, MarginLeft, Margin };

  void beginDecl() {
    nameLen_ = 0;
    nameDone_ = false;
    nameBad_ = false;
    inValue_ = false;
    prop_ = Prop::None;
    tokLen_ = 0;
    tokBad_ = false;
    important_ = false;
    tokens_ = 0;
    global_ = false;
    sawItalic_ = false;
    sawBold_ = false;
    first_ = 0;
    bigMask_ = 0;
  }

  bool nameIs(const char* s) const {
    uint8_t i = 0;
    for (; s[i] != '\0'; ++i) {
      if (i >= nameLen_ || name_[i] != static_cast<uint8_t>(s[i])) return false;
    }
    return i == nameLen_;
  }

  Prop lookupProp() const {
    if (nameBad_ || nameLen_ == 0) return Prop::None;
    if (nameIs("font-style")) return Prop::FontStyle;
    if (nameIs("font-weight")) return Prop::FontWeight;
    if (nameIs("font")) return Prop::Font;
    if (nameIs("text-align")) return Prop::TextAlign;
    if (nameIs("display")) return Prop::Display;
    if (nameIs("margin-left")) return Prop::MarginLeft;
    if (nameIs("margin")) return Prop::Margin;
    return Prop::None;
  }

  bool tokIs(const char* s) const {
    uint8_t i = 0;
    for (; s[i] != '\0'; ++i) {
      if (i >= tokLen_ || tok_[i] != static_cast<uint8_t>(s[i])) return false;
    }
    return i == tokLen_;
  }

  bool tokIsGlobal() const {
    return tokIs("inherit") || tokIs("initial") || tokIs("unset") || tokIs("revert") ||
           tokIs("revert-layer");
  }

  // Plain non-negative integer token ("700"); -1 otherwise.
  int tokNumber() const {
    if (tokLen_ == 0 || tokLen_ > 4) return -1;
    int v = 0;
    for (uint8_t i = 0; i < tokLen_; ++i) {
      if (tok_[i] < '0' || tok_[i] > '9') return -1;
      v = v * 10 + (tok_[i] - '0');
    }
    return v;
  }

  bool tokIsBold() const {
    if (tokIs("bold") || tokIs("bolder")) return true;
    const int n = tokNumber();
    return n >= 600 && n <= 1000;
  }

  // A left margin wide enough to read as a block indent (≈ 1.5em or more).
  // Integer math (×100) — the ESP32-C3 has no FPU.
  bool tokIsIndentLength() const {
    uint8_t i = 0;
    if (i < tokLen_ && tok_[i] == '+') ++i;
    uint32_t v = 0;
    bool digits = false;
    while (i < tokLen_ && tok_[i] >= '0' && tok_[i] <= '9') {
      if (v < 1000000u) v = v * 10 + static_cast<uint32_t>(tok_[i] - '0');
      digits = true;
      ++i;
    }
    v *= 100;
    if (i < tokLen_ && tok_[i] == '.') {
      ++i;
      uint32_t scale = 10;
      while (i < tokLen_ && tok_[i] >= '0' && tok_[i] <= '9') {
        v += static_cast<uint32_t>(tok_[i] - '0') * scale;
        scale /= 10;
        digits = true;
        ++i;
      }
    }
    if (!digits) return false;
    const uint8_t* u = tok_ + i;
    const uint8_t ul = static_cast<uint8_t>(tokLen_ - i);
    struct Unit {
      const char* name;
      uint32_t min100;  // threshold × 100
    };
    static constexpr Unit kUnits[] = {
        {"em", 150},  {"rem", 150}, {"px", 2000}, {"pt", 1500}, {"%", 400}, {"mm", 500}, {"cm", 50},
        {"in", 20},   {"pc", 125},  {"ex", 300},  {"ch", 300},  {"vw", 400},
    };
    for (const Unit& unit : kUnits) {
      uint8_t k = 0;
      while (unit.name[k] != '\0' && k < ul && u[k] == static_cast<uint8_t>(unit.name[k])) ++k;
      if (unit.name[k] == '\0' && k == ul) return v >= unit.min100;
    }
    return false;
  }

  // A value token ended.
  void separator() {
    if (!inValue_ || important_ || tokLen_ == 0) {
      tokLen_ = 0;
      tokBad_ = false;
      return;
    }
    const bool bad = tokBad_;
    if (tokens_ == 0) global_ = !bad && tokIsGlobal();
    if (!bad) {
      switch (prop_) {
        case Prop::FontStyle:
          if (tokens_ == 0) first_ = (tokIs("italic") || tokIs("oblique")) ? 1 : 2;
          break;
        case Prop::FontWeight:
          if (tokens_ == 0) first_ = tokIsBold() ? 1 : 2;
          break;
        case Prop::Font:
          if (tokIs("italic") || tokIs("oblique")) sawItalic_ = true;
          if (tokIsBold()) sawBold_ = true;
          break;
        case Prop::TextAlign:
          if (tokens_ == 0) {
            first_ = (tokIs("center") || tokIs("-webkit-center") || tokIs("-moz-center")) ? 1 : 2;
          }
          break;
        case Prop::Display:
          if (tokens_ == 0) first_ = tokIs("none") ? 1 : 2;
          break;
        case Prop::MarginLeft:
        case Prop::Margin:
          if (tokens_ < 4 && tokIsIndentLength()) bigMask_ = static_cast<uint8_t>(bigMask_ | (1u << tokens_));
          break;
        case Prop::None:
          break;
      }
    }
    if (tokens_ < 255) ++tokens_;
    tokLen_ = 0;
    tokBad_ = false;
  }

  void endDecl() {
    separator();
    if (inValue_ && prop_ != Prop::None && tokens_ > 0 && !global_) {
      CssEffects d;
      auto set = [&d](const uint8_t bit, const bool on) {
        d.mask = static_cast<uint8_t>(d.mask | bit);
        if (on) d.value = static_cast<uint8_t>(d.value | bit);
      };
      switch (prop_) {
        case Prop::FontStyle:
          set(kCssItalic, first_ == 1);
          break;
        case Prop::FontWeight:
          set(kCssBold, first_ == 1);
          break;
        case Prop::Font:  // the shorthand resets what it does not name
          set(kCssItalic, sawItalic_);
          set(kCssBold, sawBold_);
          break;
        case Prop::TextAlign:
          set(kCssCenter, first_ == 1);
          break;
        case Prop::Display:
          set(kCssHidden, first_ == 1);
          break;
        case Prop::MarginLeft:
          set(kCssIndent, (bigMask_ & 1u) != 0);
          break;
        case Prop::Margin: {
          // 1 value: all sides; 2–3: second is left/right; 4: top right bottom left.
          const uint8_t left = tokens_ >= 4 ? 3 : (tokens_ >= 2 ? 1 : 0);
          set(kCssIndent, (bigMask_ & (1u << left)) != 0);
          break;
        }
        case Prop::None:
          break;
      }
      fx_.apply(d);
    }
    beginDecl();
  }

  CssEffects fx_;
  uint8_t name_[12];
  uint8_t nameLen_;
  uint8_t tok_[16];
  uint8_t tokLen_;
  uint8_t tokens_;
  uint8_t first_;    // first value token: 0 none, 1 "on" keyword, 2 other
  uint8_t bigMask_;  // margin: which of the first 4 values are indent-wide
  uint8_t quote_;
  uint8_t parenDepth_;
  uint8_t comment_;
  Prop prop_;
  bool nameDone_;
  bool nameBad_;
  bool inValue_;
  bool tokBad_;
  bool important_;
  bool global_;
  bool sawItalic_;
  bool sawBold_;
  bool slash_;
};

// -----------------------------------------------------------------------------
// CssStyleTable — read-only selector-hash → effects map (sorted, binary
// search).  An empty table owns no heap.
// -----------------------------------------------------------------------------
class CssStyleTable {
 public:
  // Serialized blob: 'C' 'S' version 0, count u16 LE, then count × {hash u32
  // LE, mask u8, value u8}, hashes strictly increasing.
  static constexpr uint8_t kVersion = 1;
  static constexpr size_t kHeaderBytes = 6;
  static constexpr size_t kEntryBytes = 6;
  static constexpr uint16_t kMaxEntries = 512;

  CssStyleTable() = default;
  CssStyleTable(const CssStyleTable&) = delete;
  CssStyleTable& operator=(const CssStyleTable&) = delete;

  // Replace the contents with a serialized table.  False on a malformed blob
  // or allocation failure (the table is then empty).
  bool load(const uint8_t* data, size_t len);

  uint16_t size() const { return count_; }

  CssEffects lookup(const uint32_t hash) const {
    uint16_t lo = 0, hi = count_;
    while (lo < hi) {
      const uint16_t mid = static_cast<uint16_t>((lo + hi) / 2);
      if (hashes_[mid] < hash) lo = static_cast<uint16_t>(mid + 1);
      else hi = mid;
    }
    CssEffects fx;
    if (lo < count_ && hashes_[lo] == hash) {
      fx.mask = static_cast<uint8_t>(effects_[lo] & 0xFFu);
      fx.value = static_cast<uint8_t>(effects_[lo] >> 8);
    }
    return fx;
  }

 private:
  std::unique_ptr<uint32_t[]> hashes_;
  std::unique_ptr<uint16_t[]> effects_;  // mask | value << 8
  uint16_t count_ = 0;
};

// -----------------------------------------------------------------------------
// CssStyleSheetCompiler — streaming stylesheet → table compiler.  Feed each
// stylesheet in any chunking, call endSheet() after each, then serialize().
// Bounded: ≤ kMaxSelectorsPerRule selectors per rule, ≤ kMaxEntries entries.
// -----------------------------------------------------------------------------
class CssStyleSheetCompiler {
 public:
  static constexpr uint8_t kMaxSelectorsPerRule = 32;

  CssStyleSheetCompiler();

  void feed(const uint8_t* data, size_t len);
  void endSheet();  // flushes and resets the lexer for the next stylesheet

  // Sorted table blob (see CssStyleTable).  Indent / display off-declarations
  // that can never override an "on" are pruned first; italic / bold / centre
  // ones always stay — an element's own "off" suspends that style (inside an
  // italic paragraph, or on <i>/<h2> itself).
  void serialize(std::vector<uint8_t>& out) const;

  size_t rulesSeen() const { return rules_; }
  size_t entryCount() const { return entries_.size(); }
  size_t droppedSelectors() const { return dropped_; }

 private:
  struct Entry {
    uint32_t hash;
    CssEffects fx;
  };
  enum class State : uint8_t { Prelude, Block, AtRule };

  void byte(uint8_t b);
  void selectorByte(uint8_t b);
  void endSelector();
  void endRule();
  void resetPrelude();
  void add(uint32_t hash, CssEffects fx);

  State state_;
  // Lexer: comments are removed before anything else sees the bytes.
  bool slash_ = false;
  uint8_t comment_ = 0;
  uint8_t quote_ = 0;
  uint16_t depth_ = 0;  // brace depth inside a block / at-rule
  bool atBlock_ = false;

  // Current selector.
  uint32_t selHash_;
  uint8_t selLen_;
  bool selClass_;     // a '.' was seen
  bool selComplex_;   // anything we do not support
  bool selSpace_;     // whitespace after content (a following char = descendant)
  bool selDotLast_;   // ends in '.'
  bool selAny_;       // non-whitespace seen in this prelude
  uint32_t ruleHashes_[kMaxSelectorsPerRule];
  uint8_t ruleCount_;

  CssDeclParser decl_;
  std::vector<Entry> entries_;  // sorted by hash
  size_t rules_ = 0;
  size_t dropped_ = 0;
};

}  // namespace snapix::smolport
