#pragma once

#include <cstdint>
#include <cstring>

#include "config.h"

/**
 * Theme configuration for Snapix UI.
 *
 * Controls colors, fonts, and layout for all activities.
 * Since e-ink is binary (black/white), colors are represented as booleans:
 * - true = black (default ink color)
 * - false = white (inverted/background color)
 */
struct Theme {
  // Theme metadata
  char displayName[32];  // Human-readable name for settings UI

  // Color scheme
  bool invertedMode;  // Global dark/light mode (true = dark background)

  // Selection styles
  bool selectionFillBlack;  // Selection highlight fill color
  bool selectionTextBlack;  // Text color on selection

  // Text styles
  bool primaryTextBlack;    // Normal text color
  bool secondaryTextBlack;  // Secondary/dimmed text color

  // Background
  uint8_t backgroundColor;  // Screen clear color (0x00 = black, 0xFF = white)

  // Layout margins and spacing
  uint8_t screenMarginTop;
  uint8_t screenMarginSide;
  uint8_t itemHeight;
  uint8_t itemSpacing;
  uint8_t itemPaddingX;      // Horizontal padding inside items
  uint8_t itemValuePadding;  // Right padding for values
  uint8_t statusBarOffsetY;       // Additional downward shift for status bar
  uint8_t statusBarReservedHeight;  // Reader-content margin below baseline (overrides
                                    // the legacy 23 px reservation when non-zero —
                                    // lets small status fonts free up content space)

  // Font IDs
  int uiFontId;
  int smallFontId;
  int statusFontId;
  int readerFontIdXSmall;
  int readerFontId;
  int readerFontIdMedium;
  int readerFontIdLarge;

  // External font family names (empty = use builtin)
  char statusFontFamily[32];
  char readerFontFamilyXSmall[32];
  char readerFontFamilySmall[32];
  char readerFontFamilyMedium[32];
  char readerFontFamilyLarge[32];
};

/**
 * Get default light theme - used as default when no theme file exists.
 */
inline Theme getBuiltinLightTheme() {
  Theme theme = {};
  strncpy(theme.displayName, "Light", sizeof(theme.displayName));
  theme.invertedMode = false;
  theme.selectionFillBlack = true;
  theme.selectionTextBlack = false;
  theme.primaryTextBlack = true;
  theme.secondaryTextBlack = true;
  theme.backgroundColor = 0xFF;
  theme.screenMarginTop = 9;
  theme.screenMarginSide = 3;
  theme.itemHeight = 30;
  theme.itemSpacing = 0;
  theme.itemPaddingX = 8;
  theme.itemValuePadding = 20;
  theme.statusBarOffsetY = 0;
  theme.statusBarReservedHeight = 23;  // Legacy default sized for the small14
                                       // status font (28 px tall → fills the
                                       // reserved area).  Builtin themes with
                                       // smaller status fonts override below.
  theme.uiFontId = UI_FONT_ID;
  theme.smallFontId = SMALL_FONT_ID;
  theme.statusFontId = SMALL_FONT_ID;
  theme.readerFontIdXSmall = READER_FONT_ID_XSMALL;
  theme.readerFontId = READER_FONT_ID;
  theme.readerFontIdMedium = READER_FONT_ID_MEDIUM;
  theme.readerFontIdLarge = READER_FONT_ID_LARGE;
  theme.statusFontFamily[0] = '\0';
  theme.readerFontFamilyXSmall[0] = '\0';
  theme.readerFontFamilySmall[0] = '\0';
  theme.readerFontFamilyMedium[0] = '\0';
  theme.readerFontFamilyLarge[0] = '\0';
  return theme;
}

/**
 * Get default dark theme - inverted colors.
 */
inline Theme getBuiltinDarkTheme() {
  Theme theme = {};
  strncpy(theme.displayName, "Dark", sizeof(theme.displayName));
  theme.invertedMode = true;
  theme.selectionFillBlack = false;
  theme.selectionTextBlack = true;
  theme.primaryTextBlack = false;
  theme.secondaryTextBlack = false;
  theme.backgroundColor = 0x00;
  theme.screenMarginTop = 9;
  theme.screenMarginSide = 3;
  theme.itemHeight = 30;
  theme.itemSpacing = 0;
  theme.itemPaddingX = 8;
  theme.itemValuePadding = 20;
  theme.statusBarOffsetY = 0;
  theme.statusBarReservedHeight = 23;  // Legacy default sized for the small14
                                       // status font (28 px tall → fills the
                                       // reserved area).  Builtin themes with
                                       // smaller status fonts override below.
  theme.uiFontId = UI_FONT_ID;
  theme.smallFontId = SMALL_FONT_ID;
  theme.statusFontId = SMALL_FONT_ID;
  theme.readerFontIdXSmall = READER_FONT_ID_XSMALL;
  theme.readerFontId = READER_FONT_ID;
  theme.readerFontIdMedium = READER_FONT_ID_MEDIUM;
  theme.readerFontIdLarge = READER_FONT_ID_LARGE;
  theme.statusFontFamily[0] = '\0';
  theme.readerFontFamilyXSmall[0] = '\0';
  theme.readerFontFamilySmall[0] = '\0';
  theme.readerFontFamilyMedium[0] = '\0';
  theme.readerFontFamilyLarge[0] = '\0';
  return theme;
}

/**
 * "JetBrains Mono" builtin theme — uses the embedded JetBrains Mono NL
 * derivative (regular only, sizes 4/10/11/12/13).  Selectable from Settings
 * without any SD .theme file.  OFL.txt does not declare a Reserved Font
 * Name (per §28-37 RFN must be specified explicitly after the copyright
 * statement), so OFL §63-66 does not restrict naming here; the original
 * font is credited in README under "Third-party fonts".
 */
inline Theme getBuiltinMonoTheme() {
  Theme theme = getBuiltinLightTheme();
  strncpy(theme.displayName, "JetBrains Mono", sizeof(theme.displayName));
  theme.statusFontId = JETBRAINS_MONO_4_FONT_ID;
  theme.readerFontIdXSmall = JETBRAINS_MONO_10_FONT_ID;
  theme.readerFontId = JETBRAINS_MONO_11_FONT_ID;
  theme.readerFontIdMedium = JETBRAINS_MONO_12_FONT_ID;
  theme.readerFontIdLarge = JETBRAINS_MONO_13_FONT_ID;
  // Status bar text bottom anchored at screenH (touches bottom edge), with
  // a tight reserved height so the reader gets ~12 extra pixels of content
  // area — enough to cross the 25 px line-height threshold and fit one more
  // line in compact spacing.  jetbrains_mono_4 height = 12; reserved = 11
  // (12 − 1 = textHeight − 1); offset = 4 (small breathing room above text
  // within the reserved area).  textY = 800 − 14 − 2 + 4 = 788, same as the
  // previous offset=16/reserved=23 layout — visual position unchanged.
  theme.statusBarOffsetY = 4;
  theme.statusBarReservedHeight = 11;
  return theme;
}

/**
 * "PT Mono" — geometric monospace by ParaType, optimised for Cyrillic.
 * Status bar offset: small14 height 28, pt_mono_4 height 10, offset = 18.
 */
inline Theme getBuiltinPtMonoTheme() {
  Theme theme = getBuiltinLightTheme();
  strncpy(theme.displayName, "PT Mono", sizeof(theme.displayName));
  theme.statusFontId = PT_MONO_4_FONT_ID;
  theme.readerFontIdXSmall = PT_MONO_10_FONT_ID;
  theme.readerFontId = PT_MONO_11_FONT_ID;
  theme.readerFontIdMedium = PT_MONO_12_FONT_ID;
  theme.readerFontIdLarge = PT_MONO_13_FONT_ID;
  // pt_mono_4 height = 10; reserved = 9; offset = 4 → text bottom at 800.
  theme.statusBarOffsetY = 4;
  theme.statusBarReservedHeight = 9;
  return theme;
}

/**
 * "IBM Plex Mono" — humanist monospace by IBM.  Status bar offset 16
 * (same as JetBrains Mono — both have size-4 ascender 9, descender -3).
 */
inline Theme getBuiltinIbmPlexMonoTheme() {
  Theme theme = getBuiltinLightTheme();
  strncpy(theme.displayName, "IBM Plex Mono", sizeof(theme.displayName));
  theme.statusFontId = IBM_PLEX_MONO_4_FONT_ID;
  theme.readerFontIdXSmall = IBM_PLEX_MONO_10_FONT_ID;
  theme.readerFontId = IBM_PLEX_MONO_11_FONT_ID;
  theme.readerFontIdMedium = IBM_PLEX_MONO_12_FONT_ID;
  theme.readerFontIdLarge = IBM_PLEX_MONO_13_FONT_ID;
  // ibm_plex_mono_4 height = 12 (same as jetbrains_mono_4); reserved = 11; offset = 4.
  theme.statusBarOffsetY = 4;
  theme.statusBarReservedHeight = 11;
  return theme;
}

/**
 * "Literata" — serif designed for ebook reading by Type Network.  Status
 * bar offset 15 (size-4 ascender 10, descender -3, height 13).
 */
inline Theme getBuiltinLiterataTheme() {
  Theme theme = getBuiltinLightTheme();
  strncpy(theme.displayName, "Literata", sizeof(theme.displayName));
  theme.statusFontId = LITERATA_4_FONT_ID;
  theme.readerFontIdXSmall = LITERATA_10_FONT_ID;
  theme.readerFontId = LITERATA_11_FONT_ID;
  theme.readerFontIdMedium = LITERATA_12_FONT_ID;
  theme.readerFontIdLarge = LITERATA_13_FONT_ID;
  // literata_4 height = 13; reserved = 12; offset = 4.
  theme.statusBarOffsetY = 4;
  theme.statusBarReservedHeight = 12;
  return theme;
}

// For use in initialization
#define BUILTIN_LIGHT_THEME getBuiltinLightTheme()
#define BUILTIN_DARK_THEME getBuiltinDarkTheme()
#define BUILTIN_MONO_THEME getBuiltinMonoTheme()
#define BUILTIN_PT_MONO_THEME getBuiltinPtMonoTheme()
#define BUILTIN_IBM_PLEX_MONO_THEME getBuiltinIbmPlexMonoTheme()
#define BUILTIN_LITERATA_THEME getBuiltinLiterataTheme()
