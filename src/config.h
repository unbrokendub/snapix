#pragma once

/**
 * Generated with:
 *  ruby -rdigest -e 'puts [
 *    "./lib/EpdFont/builtinFonts/reader_xsmall_regular_2b.h",
 *    "./lib/EpdFont/builtinFonts/reader_xsmall_bold_2b.h",
 *    "./lib/EpdFont/builtinFonts/reader_xsmall_italic_2b.h",
 *  ].map{|f| Digest::SHA256.hexdigest(File.read(f)).to_i(16) }.sum % (2 ** 32) - (2 ** 31)'
 */
#define READER_FONT_ID_XSMALL (-1054355880)

/**
 * Generated with:
 *  ruby -rdigest -e 'puts [
 *    "./lib/EpdFont/builtinFonts/reader_2b.h",
 *    "./lib/EpdFont/builtinFonts/reader_bold_2b.h",
 *    "./lib/EpdFont/builtinFonts/reader_italic_2b.h",
 *  ].map{|f| Digest::SHA256.hexdigest(File.read(f)).to_i(16) }.sum % (2 ** 32) - (2 ** 31)'
 */
#define READER_FONT_ID 805076859

/**
 * Generated with:
 *  ruby -rdigest -e 'puts [
 *    "./lib/EpdFont/builtinFonts/reader_medium_2b.h",
 *    "./lib/EpdFont/builtinFonts/reader_medium_bold_2b.h",
 *    "./lib/EpdFont/builtinFonts/reader_medium_italic_2b.h",
 *  ].map{|f| Digest::SHA256.hexdigest(File.read(f)).to_i(16) }.sum % (2 ** 32) - (2 ** 31)'
 */
#define READER_FONT_ID_MEDIUM 1664483350

/**
 * Generated with:
 *  ruby -rdigest -e 'puts [
 *    "./lib/EpdFont/builtinFonts/reader_large_2b.h",
 *    "./lib/EpdFont/builtinFonts/reader_large_bold_2b.h",
 *    "./lib/EpdFont/builtinFonts/reader_large_italic_2b.h",
 *  ].map{|f| Digest::SHA256.hexdigest(File.read(f)).to_i(16) }.sum % (2 ** 32) - (2 ** 31)'
 */
#define READER_FONT_ID_LARGE 1574539415

/**
 * Generated with:
 *  ruby -rdigest -e 'puts [
 *    "./lib/EpdFont/builtinFonts/ui_12.h",
 *    "./lib/EpdFont/builtinFonts/ui_bold_12.h",
 *  ].map{|f| Digest::SHA256.hexdigest(File.read(f)).to_i(16) }.sum % (2 ** 32) - (2 ** 31)'
 */
#define UI_FONT_ID 133654340

/**
 * Generated with:
 *  ruby -rdigest -e 'puts [
 *    "./lib/EpdFont/builtinFonts/small14.h",
 *  ].map{|f| Digest::SHA256.hexdigest(File.read(f)).to_i(16) }.sum % (2 ** 32) - (2 ** 31)'
 */
#define SMALL_FONT_ID 96157773

// Embedded "Snapix Mono" font family — Snapix derivative of JetBrains Mono NL
// (SIL OFL 1.1; see scripts/jetbrains/OFL.txt).  Sizes 4 / 10 / 11 / 12 / 13
// are baked into the firmware so font rendering survives SD failures and
// removes per-glyph SD I/O on cold-render paths.  Generated via:
//   ruby -rdigest -e 'puts (Digest::SHA256.hexdigest(File.read(F)).to_i(16) %
//                          (2**32) - (2**31))'
#define JETBRAINS_MONO_4_FONT_ID  (-536722457)
#define JETBRAINS_MONO_5_FONT_ID  1025770491
#define JETBRAINS_MONO_10_FONT_ID 1386934901
#define JETBRAINS_MONO_11_FONT_ID 1652182926
#define JETBRAINS_MONO_12_FONT_ID 419397466
#define JETBRAINS_MONO_13_FONT_ID 2006126113

// Embedded "PT Mono" — ParaType Ltd. derivative, 5 sizes, regular only.
// SIL OFL 1.1; see scripts/pt-mono/OFL.txt.
#define PT_MONO_4_FONT_ID         394929841
#define PT_MONO_5_FONT_ID         (-1891215988)
#define PT_MONO_10_FONT_ID        (-169993709)
#define PT_MONO_11_FONT_ID        1597081734
#define PT_MONO_12_FONT_ID        1746552986
#define PT_MONO_13_FONT_ID        (-1556258063)

// Embedded "IBM Plex Mono" — IBM Corp. (Mike Abbink, Bold Monday) derivative.
// SIL OFL 1.1; see scripts/ibm-plex-mono/OFL.txt.
#define IBM_PLEX_MONO_4_FONT_ID   1613856708
#define IBM_PLEX_MONO_5_FONT_ID   909346825
#define IBM_PLEX_MONO_10_FONT_ID  (-1624760916)
#define IBM_PLEX_MONO_11_FONT_ID  (-1570173100)
#define IBM_PLEX_MONO_12_FONT_ID  (-1563447629)
#define IBM_PLEX_MONO_13_FONT_ID  (-535477073)

// Embedded "Literata" — Type Network for Google Fonts derivative.
// SIL OFL 1.1; see scripts/literata/OFL.txt.
#define LITERATA_4_FONT_ID        (-897024207)
#define LITERATA_5_FONT_ID        (-556886400)
#define LITERATA_10_FONT_ID       938590645
#define LITERATA_11_FONT_ID       (-1210268069)
#define LITERATA_12_FONT_ID       (-1241594796)
#define LITERATA_13_FONT_ID       (-1524126485)

// System directory for settings/state/wifi — lives on SD card next to user
// books.  Persistent across reflash, survives LittleFS format.
#define SNAPIX_DIR "/.snapix"
#define SNAPIX_SETTINGS_FILE SNAPIX_DIR "/settings.bin"
#define SNAPIX_STATE_FILE SNAPIX_DIR "/state.bin"
#define SNAPIX_WIFI_FILE SNAPIX_DIR "/wifi.bin"

// Page-cache root — v2.0.60 moved to LittleFS (internal flash).  Image BMPs
// already lived under /img/ on LittleFS since v2.0.53; the page-cache split
// off SD eliminates the SPI bus contention between display refresh and
// page-cache reads.  Path is RELATIVE to LittleFS root, NOT under SNAPIX_DIR
// (LittleFS is a separate filesystem with its own root).
#define SNAPIX_CACHE_DIR "/cache"

// Thumbnail dimensions for home screen
#define THUMB_WIDTH 320
#define THUMB_HEIGHT 440

// User configuration directory
#define CONFIG_DIR "/config"
#define CONFIG_CALIBRE_FILE CONFIG_DIR "/calibre.ini"
#define CONFIG_THEMES_DIR CONFIG_DIR "/themes"
#define CONFIG_FONTS_DIR CONFIG_DIR "/fonts"

// Calibre sync settings
#define CALIBRE_BOOKS_DIR "/Calibre"
#define CALIBRE_PORT 9090
#define CALIBRE_PROCESS_TIMEOUT_MS 50

// Applies custom theme fonts for the currently selected font size.
// Call this after font size or theme changes to reload fonts.
void applyThemeFonts();
