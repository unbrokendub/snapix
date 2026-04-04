#pragma once

#include <EpdFontFamily.h>

#include <cstring>

// Minimal mock GfxRenderer for ParsedText unit tests.
// Returns deterministic metrics: 6px per character, 4px space width.
class GfxRenderer {
 public:
  int getSpaceWidth(int) const { return 4; }

  int getTextWidth(int, const char* text, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return static_cast<int>(strlen(text)) * 6;
  }

  int getLineHeight(int) const { return 20; }
};
