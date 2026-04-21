#pragma once

#include <EpdFontFamily.h>

#include <cstring>

// Minimal mock GfxRenderer for ParsedText unit tests.
// Returns deterministic metrics: 6px per character, 4px space width.
class GfxRenderer {
 public:
  int getSpaceWidth(int) const { return 4; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 4; }

  int getTextWidth(int, const char* text, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return static_cast<int>(strlen(text)) * 6;
  }

  int getTextAdvanceWidth(int, const char* text, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return static_cast<int>(strlen(text)) * 6;
  }

  int getLineHeight(int) const { return 20; }

  // TextBlock::render() / warmGlyphs() stubs — not exercised by layout tests
  // but required for linking now that TextBlock.cpp is compiled in.
  void drawText(int, int, int, const char*, bool = true,
                EpdFontFamily::Style = EpdFontFamily::REGULAR) const {}
  int getTextAdvanceWidth(int, const char*, EpdFontFamily::Style) { return 0; }
  void warmTextGlyphs(int, const char*, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {}
};
