#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "CssStyle.h"

/**
 * CssParser - Simple CSS parser for extracting supported properties
 *
 * Handles:
 * - Class selectors (.classname)
 * - Element.class selectors (p.classname)
 * - Tag selectors (p, div, etc.)
 * - Multiple selectors separated by commas
 * - Inline styles
 *
 * Limitations:
 * - Does not support complex selectors (descendant, child, etc.)
 * - Does not support pseudo-classes or pseudo-elements
 * - Only extracts properties we actually use
 */
class CssParser {
 public:
  CssParser();
  ~CssParser();

  /**
   * Parse a CSS file and add its rules to the style map
   * Returns true if parsing was successful
   */
  bool parseFile(const char* filepath);

  /**
   * Get the style for a given selector (class or tag)
   * Returns nullptr if no style is defined
   */
  const CssStyle* getStyleForClass(const std::string& className) const;

  /**
   * Get the style for a tag name (e.g., "p", "div")
   */
  CssStyle getTagStyle(const std::string& tagName) const;

  /**
   * Get the combined style for a tag with multiple class names (space-separated)
   * Styles are merged in order, later classes override earlier ones
   */
  CssStyle getCombinedStyle(const std::string& tagName, const std::string& classNames) const;

  /**
   * Parse an inline style attribute (e.g., "text-align: center; font-weight: bold;")
   * Returns a CssStyle with the parsed properties
   * Static method - can be called without a CssParser instance
   */
  static CssStyle parseInlineStyle(const std::string& styleAttr);

  bool hasStyles() const { return !styleMap_.empty(); }
  size_t getStyleCount() const { return styleMap_.size(); }
  void clear() {
    styleMap_.clear();
    arena_.clear();
  }

 private:
  void parseRule(const std::string& selector, const std::string& properties);
  static void parseProperty(const std::string& name, const std::string& value, CssStyle& style);
  static TextAlign parseTextAlign(const std::string& value);
  static CssFontStyle parseFontStyle(const std::string& value);
  static CssFontWeight parseFontWeight(const std::string& value);

  static constexpr size_t MAX_CSS_RULES = 512;
  static constexpr size_t MAX_CSS_SELECTOR_LENGTH = 256;
  static constexpr size_t MAX_CSS_FILE_SIZE = 64 * 1024;

  // v2.0.181 — string-arena refactor.  Pre-fix each entry was
  //   std::pair<std::string, CssStyle>
  // = 24 B string struct + (heap chars for selectors > SSO limit ~15)
  //   + 8 B CssStyle + alignment = ~32-72 B per entry.
  // For 43 selectors (typical EPUB), that's ~1.5-3 KB of per-book
  // metadata in heap with std::string fragments scattered through it.
  //
  // Now each entry is:
  //   StyleEntry { uint16_t offset; uint16_t length; CssStyle style }
  // = 4 B offset+length + 8 B style + padding = 12 B fixed.
  // Selector character bytes are all concatenated into a single
  // contiguous `arena_` vector.  For 43 selectors averaging 10 chars
  // that's 43 * 12 + 430 = ~970 B total vs ~1.5-3 KB before — saves
  // ~500 B (mostly-SSO selectors) to ~2 KB (mostly-heap selectors).
  //
  // Lookup performance unchanged: still O(log N) binary search via
  // std::lower_bound, comparison done via memcmp on arena slices.
  //
  // External API (getStyleForClass / getTagStyle / getCombinedStyle)
  // takes std::string by const-ref as before — no callsite changes.
  //
  // The arena is also cleared together with the style map on clear(),
  // and grows monotonically during parsing (no removals).  Realloc is
  // bounded by MAX_CSS_FILE_SIZE = 64 KB — vector::reserve() in
  // parseFile() would let us pre-size if growth churn becomes a
  // problem, but for typical EPUBs the doubling growth strategy
  // settles within 1-2 reallocs.
  struct StyleEntry {
    uint16_t offset;
    uint16_t length;
    CssStyle style;
  };
  std::vector<char> arena_;
  std::vector<StyleEntry> styleMap_;

  // Lexicographic compare: arena slice [offset..offset+length) vs key.
  // Returns <0 / 0 / >0 like memcmp / strcmp.
  int compareSliceToKey(const StyleEntry& entry, const char* keyData, size_t keyLen) const {
    const size_t minLen = entry.length < keyLen ? entry.length : keyLen;
    const int diff = std::memcmp(arena_.data() + entry.offset, keyData, minLen);
    if (diff != 0) return diff;
    if (entry.length < keyLen) return -1;
    if (entry.length > keyLen) return 1;
    return 0;
  }

  // Binary search helper — returns iterator to matching entry or end().
  std::vector<StyleEntry>::const_iterator findStyle(const std::string& key) const {
    auto it = std::lower_bound(
        styleMap_.begin(), styleMap_.end(), key,
        [this](const StyleEntry& entry, const std::string& k) {
          return compareSliceToKey(entry, k.data(), k.size()) < 0;
        });
    if (it != styleMap_.end() && compareSliceToKey(*it, key.data(), key.size()) == 0) return it;
    return styleMap_.end();
  }
};
