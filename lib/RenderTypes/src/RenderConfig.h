#pragma once
#include <cmath>
#include <cstdint>

struct RenderConfig {
  int fontId = 0;
  float lineCompression = 0.0f;
  uint8_t indentLevel = 0;
  uint8_t spacingLevel = 0;
  uint8_t paragraphAlignment = 0;
  bool hyphenation = false;
  bool showImages = false;
  bool bionicReading = false;
  uint8_t fakeBold = 0;  // 0=off, 1=bold (+1 shift), 2=extrabold (-1/+1 shift)
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  // v3.6.0 — built-in font id used for <sup>/<sub> (a smaller face than
  // `fontId`).  Set by Settings::getRenderConfig; the streaming paginator's
  // adapter renders super/subscript words in this font, top/bottom-aligned.
  // 0 = fall back to `fontId` (no shrink).  Part of the layout config so the
  // MEASURE walk and render agree on super/sub word widths.
  int superSubFontId = 0;

  RenderConfig() = default;
  RenderConfig(int fontId, float lineCompression, uint8_t indentLevel, uint8_t spacingLevel, uint8_t paragraphAlignment,
               bool hyphenation, bool showImages, bool bionicReading, uint8_t fakeBold, uint16_t viewportWidth,
               uint16_t viewportHeight, int superSubFontId = 0)
      : fontId(fontId),
        lineCompression(lineCompression),
        indentLevel(indentLevel),
        spacingLevel(spacingLevel),
        paragraphAlignment(paragraphAlignment),
        hyphenation(hyphenation),
        showImages(showImages),
        bionicReading(bionicReading),
        fakeBold(fakeBold),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        superSubFontId(superSubFontId) {}

  bool operator==(const RenderConfig& o) const {
    return fontId == o.fontId && std::abs(lineCompression - o.lineCompression) < 1e-6f &&
           indentLevel == o.indentLevel && spacingLevel == o.spacingLevel &&
           paragraphAlignment == o.paragraphAlignment && hyphenation == o.hyphenation && showImages == o.showImages &&
           bionicReading == o.bionicReading && fakeBold == o.fakeBold &&
           viewportWidth == o.viewportWidth && viewportHeight == o.viewportHeight &&
           superSubFontId == o.superSubFontId;
  }
  bool operator!=(const RenderConfig& o) const { return !(*this == o); }
};
