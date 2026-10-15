#include "CssStyleTable.h"

#include <algorithm>
#include <new>

namespace snapix::smolport {

namespace {

inline bool isCssWhitespace(const uint8_t b) {
  return b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == '\f';
}

inline bool isAsciiAlpha(const uint8_t b) { return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z'); }

// Identifier byte of a tag or class name (non-ASCII allowed, escapes are not).
inline bool isIdentByte(const uint8_t b) {
  return isAsciiAlpha(b) || (b >= '0' && b <= '9') || b == '-' || b == '_' || b >= 0x80;
}

}  // namespace

// ===========================================================================
// CssStyleTable
// ===========================================================================
bool CssStyleTable::load(const uint8_t* data, const size_t len) {
  hashes_.reset();
  effects_.reset();
  count_ = 0;
  if (data == nullptr || len < kHeaderBytes || data[0] != 'C' || data[1] != 'S' || data[2] != kVersion) {
    return false;
  }
  const uint16_t count = static_cast<uint16_t>(data[4] | (data[5] << 8));
  if (count > kMaxEntries || len != kHeaderBytes + static_cast<size_t>(count) * kEntryBytes) return false;
  if (count == 0) return true;
  std::unique_ptr<uint32_t[]> hashes(new (std::nothrow) uint32_t[count]);
  std::unique_ptr<uint16_t[]> effects(new (std::nothrow) uint16_t[count]);
  if (!hashes || !effects) return false;
  const uint8_t* p = data + kHeaderBytes;
  for (uint16_t i = 0; i < count; ++i, p += kEntryBytes) {
    const uint32_t h = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    if (i > 0 && h <= hashes[i - 1]) return false;  // must be strictly sorted
    hashes[i] = h;
    effects[i] = static_cast<uint16_t>(p[4] | (p[5] << 8));
  }
  hashes_ = std::move(hashes);
  effects_ = std::move(effects);
  count_ = count;
  return true;
}

// ===========================================================================
// CssStyleSheetCompiler
// ===========================================================================
CssStyleSheetCompiler::CssStyleSheetCompiler() : state_(State::Prelude) { resetPrelude(); }

void CssStyleSheetCompiler::feed(const uint8_t* data, const size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const uint8_t b = data[i];
    // Comments are removed before anything else sees the bytes (CSS: a
    // comment is not whitespace, `p/**/.a` is `p.a`).
    if (comment_ != 0) {
      if (comment_ == 2 && b == '/') comment_ = 0;
      else comment_ = (b == '*') ? 2 : 1;
      continue;
    }
    if (slash_) {
      slash_ = false;
      if (b == '*' && quote_ == 0) {
        comment_ = 1;
        continue;
      }
      byte('/');
    }
    if (b == '/' && quote_ == 0) {
      slash_ = true;
      continue;
    }
    byte(b);
  }
}

void CssStyleSheetCompiler::endSheet() {
  if (slash_ && state_ == State::Block) byte('/');
  if (state_ == State::Block && depth_ == 1) endRule();  // unterminated last rule
  state_ = State::Prelude;
  slash_ = false;
  comment_ = 0;
  depth_ = 0;
  atBlock_ = false;
  decl_.reset();
  resetPrelude();
}

void CssStyleSheetCompiler::byte(const uint8_t b) {
  switch (state_) {
    case State::Prelude:
      if (quote_ != 0) {  // [title="x"] — already complex
        if (b == quote_) quote_ = 0;
        return;
      }
      if (b == '"' || b == '\'') {
        quote_ = b;
        selComplex_ = true;
        selAny_ = true;
        return;
      }
      if (b == '{') {
        endSelector();
        state_ = State::Block;
        depth_ = 1;
        decl_.reset();
        return;
      }
      if (b == '@' && !selAny_) {
        state_ = State::AtRule;
        depth_ = 0;
        atBlock_ = false;
        return;
      }
      if (b == ';' || b == '}') {  // stray — drop the prelude
        resetPrelude();
        return;
      }
      if (b == ',') {
        endSelector();
        return;
      }
      selectorByte(b);
      return;

    case State::Block:
      if (quote_ != 0) {
        if (b == quote_) quote_ = 0;
        if (depth_ == 1) decl_.feed(b);
        return;
      }
      if (b == '"' || b == '\'') {
        quote_ = b;
        if (depth_ == 1) decl_.feed(b);
        return;
      }
      if (b == '{') {  // nested rule (CSS nesting) — skipped
        if (depth_ < 0xFFFF) ++depth_;
        return;
      }
      if (b == '}') {
        if (--depth_ == 0) {
          endRule();
          state_ = State::Prelude;
          resetPrelude();
        }
        return;
      }
      if (depth_ == 1) decl_.feed(b);
      return;

    case State::AtRule:
      // @media / @supports / @font-face / @page … { … } and @import …; are
      // skipped whole — device-specific overrides live there (amzn-kf8 …).
      if (quote_ != 0) {
        if (b == quote_) quote_ = 0;
        return;
      }
      if (b == '"' || b == '\'') {
        quote_ = b;
        return;
      }
      if (b == '{') {
        atBlock_ = true;
        if (depth_ < 0xFFFF) ++depth_;
        return;
      }
      if (b == '}') {
        if (depth_ > 0) --depth_;
        if (depth_ == 0) {
          state_ = State::Prelude;
          resetPrelude();
        }
        return;
      }
      if (b == ';' && depth_ == 0) {
        state_ = State::Prelude;
        resetPrelude();
      }
      return;
  }
}

// Accepts `tag`, `.class` and `tag.class`; anything else marks the selector
// complex and it is dropped at endSelector().
void CssStyleSheetCompiler::selectorByte(const uint8_t b) {
  if (isCssWhitespace(b)) {
    if (selLen_ > 0) selSpace_ = true;
    return;
  }
  selAny_ = true;
  if (selSpace_) selComplex_ = true;  // descendant combinator
  if (selComplex_) return;
  if (b == '.') {
    if (selClass_) {  // .a.b — compound classes unsupported
      selComplex_ = true;
      return;
    }
    selClass_ = true;
    selDotLast_ = true;
    selHash_ = cssHash(selHash_, '.');
    if (selLen_ < 255) ++selLen_;
    return;
  }
  if (isIdentByte(b)) {
    if (!selClass_ && selLen_ == 0 && !isAsciiAlpha(b)) {  // a tag name starts with a letter
      selComplex_ = true;
      return;
    }
    selHash_ = cssHash(selHash_, selClass_ ? b : cssLower(b));  // classes are case-sensitive
    selDotLast_ = false;
    if (selLen_ < 255) ++selLen_;
    return;
  }
  selComplex_ = true;  // # : [ > + ~ * | ( \ …
}

void CssStyleSheetCompiler::endSelector() {
  if (selLen_ > 0 && !selComplex_ && !selDotLast_) {
    if (ruleCount_ < kMaxSelectorsPerRule) ruleHashes_[ruleCount_++] = selHash_;
    else ++dropped_;
  }
  selHash_ = kCssHashSeed;
  selLen_ = 0;
  selClass_ = false;
  selComplex_ = false;
  selSpace_ = false;
  selDotLast_ = false;
}

void CssStyleSheetCompiler::resetPrelude() {
  ruleCount_ = 0;
  quote_ = 0;
  selAny_ = false;
  endSelector();
  ruleCount_ = 0;
}

void CssStyleSheetCompiler::endRule() {
  const CssEffects fx = decl_.finish();
  ++rules_;
  if (fx.mask == 0) return;
  for (uint8_t i = 0; i < ruleCount_; ++i) add(ruleHashes_[i], fx);
}

void CssStyleSheetCompiler::add(const uint32_t hash, const CssEffects fx) {
  auto it = std::lower_bound(entries_.begin(), entries_.end(), hash,
                             [](const Entry& e, const uint32_t h) { return e.hash < h; });
  if (it != entries_.end() && it->hash == hash) {
    it->fx.apply(fx);  // same selector again: later declarations win
    return;
  }
  if (entries_.size() >= CssStyleTable::kMaxEntries) {
    ++dropped_;
    return;
  }
  entries_.insert(it, Entry{hash, fx});
}

void CssStyleSheetCompiler::serialize(std::vector<uint8_t>& out) const {
  // An "off" indent / display declaration only matters when it overrides an
  // "on" from a less specific selector of the same element — impossible for a
  // property no rule ever turns on.  Italic / bold / centre off-declarations
  // always stay: they also suspend tags and ancestors (`<i class="roman">`).
  uint8_t onBits = kCssItalic | kCssBold | kCssCenter;
  for (const Entry& e : entries_) onBits = static_cast<uint8_t>(onBits | e.fx.value);

  out.clear();
  out.reserve(CssStyleTable::kHeaderBytes + entries_.size() * CssStyleTable::kEntryBytes);
  out.push_back('C');
  out.push_back('S');
  out.push_back(CssStyleTable::kVersion);
  out.push_back(0);
  out.push_back(0);  // count, patched below
  out.push_back(0);
  uint16_t count = 0;
  for (const Entry& e : entries_) {
    const uint8_t mask = static_cast<uint8_t>(e.fx.mask & (e.fx.value | onBits));
    if (mask == 0) continue;
    const uint8_t value = static_cast<uint8_t>(e.fx.value & mask);
    for (int s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>((e.hash >> s) & 0xFFu));
    out.push_back(mask);
    out.push_back(value);
    ++count;
  }
  out[4] = static_cast<uint8_t>(count & 0xFFu);
  out[5] = static_cast<uint8_t>(count >> 8);
}

}  // namespace snapix::smolport
