#include "GfxRendererPaginatorAdapter.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <LittleFS.h>

#include <cstring>
#include <utility>

namespace snapix::smolport {

GfxRendererPaginatorAdapter::GfxRendererPaginatorAdapter(const GfxRenderer& renderer, int bodyFontId,
                                                          int headingFontId, bool pixelState,
                                                          ResolveImagePathFn resolveImagePath,
                                                          uint8_t fakeBoldMode)
    : renderer_(renderer),
      bodyFontId_(bodyFontId),
      headingFontId_(headingFontId),
      pixelState_(pixelState),
      resolveImagePath_(std::move(resolveImagePath)),
      fakeBoldMode_(fakeBoldMode) {}

// v2.0.146 — fake-bold substitution.  Returns true (and rewrites *style)
// when the input style is BOLD/BOLD_ITALIC and fakeBoldMode_ > 0.  In
// that case the caller draws REGULAR/ITALIC at multiple x-offsets
// instead of using the real bold font.
bool GfxRendererPaginatorAdapter::applyFakeBoldSubstitution(EpdFontFamily::Style* style) const {
  if (fakeBoldMode_ == 0 || style == nullptr) return false;
  if (*style == EpdFontFamily::BOLD) {
    *style = EpdFontFamily::REGULAR;
    return true;
  }
  if (*style == EpdFontFamily::BOLD_ITALIC) {
    *style = EpdFontFamily::ITALIC;
    return true;
  }
  return false;
}

void GfxRendererPaginatorAdapter::resolveStyle(uint8_t styleBits, int* outFontId,
                                                EpdFontFamily::Style* outStyle) const {
  // Heading wins over inline styles for fontId selection — same convention
  // as the legacy ChapterHtmlSlimParser layout.
  if (styleBits & kStyleHeading) {
    *outFontId = headingFontId_;
  } else {
    *outFontId = bodyFontId_;
  }

  // Bold + italic combine into BOLD_ITALIC; either alone selects the
  // single-style font.  The font family resolver inside GfxRenderer falls
  // back to REGULAR if a particular variant isn't loaded — same as the
  // legacy text path, so unknown combinations render in the regular face.
  const bool bold = (styleBits & kStyleBold) != 0;
  const bool italic = (styleBits & kStyleItalic) != 0;
  if (bold && italic) {
    *outStyle = EpdFontFamily::BOLD_ITALIC;
  } else if (bold) {
    *outStyle = EpdFontFamily::BOLD;
  } else if (italic) {
    *outStyle = EpdFontFamily::ITALIC;
  } else {
    *outStyle = EpdFontFamily::REGULAR;
  }
}

size_t GfxRendererPaginatorAdapter::copyTermBytes(const uint8_t* text, size_t len, char* outBuf,
                                                   size_t outBufSize) {
  if (outBufSize == 0) return 0;
  size_t toCopy = len;
  if (toCopy >= outBufSize) toCopy = outBufSize - 1;
  if (text != nullptr && toCopy > 0) {
    std::memcpy(outBuf, text, toCopy);
  }
  outBuf[toCopy] = '\0';
  return toCopy;
}

uint16_t GfxRendererPaginatorAdapter::measureWidth(const uint8_t* text, size_t len, uint8_t styleBits) {
  if (len == 0) return 0;
  int fontId;
  EpdFontFamily::Style style;
  resolveStyle(styleBits, &fontId, &style);

  // v2.0.146 — under fakeBold, BOLD/BOLD_ITALIC are drawn as REGULAR/
  // ITALIC with multi-pass x-offsets.  Measure against the SUBSTITUTED
  // style so the width matches what we'll actually paint.  (The 1-2 px
  // extra ink the multi-pass adds is below typographic significance for
  // layout — same trade-off the legacy TextBlock makes.)
  applyFakeBoldSubstitution(&style);

  char buf[kWordBufBytes];
  copyTermBytes(text, len, buf, sizeof(buf));
  const int advance = renderer_.getTextAdvanceWidth(fontId, buf, style);
  if (advance < 0) return 0;
  if (advance > UINT16_MAX) return UINT16_MAX;
  return static_cast<uint16_t>(advance);
}

uint16_t GfxRendererPaginatorAdapter::getSpaceWidth(uint8_t styleBits) {
  int fontId;
  EpdFontFamily::Style style;
  resolveStyle(styleBits, &fontId, &style);
  // v2.0.146 — space width should match the substituted style too.
  applyFakeBoldSubstitution(&style);
  const int width = renderer_.getSpaceWidth(fontId, style);
  if (width < 0) return 0;
  if (width > UINT16_MAX) return UINT16_MAX;
  return static_cast<uint16_t>(width);
}

void GfxRendererPaginatorAdapter::drawWord(uint16_t x, uint16_t y, const uint8_t* text, size_t len,
                                            uint8_t styleBits) {
  if (len == 0) return;
  int fontId;
  EpdFontFamily::Style style;
  resolveStyle(styleBits, &fontId, &style);

  char buf[kWordBufBytes];
  copyTermBytes(text, len, buf, sizeof(buf));

  // v2.0.146 — fake-bold multi-pass.  Mirrors the legacy
  // TextBlock::render path: when fakeBold is on and the requested
  // style is BOLD/BOLD_ITALIC, substitute REGULAR/ITALIC and
  // draw 2× (mode=1, at x and x+1) or 3× (mode=2, at x-1, x,
  // x+1).  All passes use the same pixelState_/fontId so the
  // visual outcome is a thicker version of the regular glyph.
  const bool fakeBold = applyFakeBoldSubstitution(&style);
  if (fakeBold) {
    const int yi = static_cast<int>(y);
    const int xi = static_cast<int>(x);
    if (fakeBoldMode_ == 2) {
      renderer_.drawText(fontId, xi - 1, yi, buf, pixelState_, style);
      renderer_.drawText(fontId, xi,     yi, buf, pixelState_, style);
      renderer_.drawText(fontId, xi + 1, yi, buf, pixelState_, style);
    } else {  // mode 1
      renderer_.drawText(fontId, xi,     yi, buf, pixelState_, style);
      renderer_.drawText(fontId, xi + 1, yi, buf, pixelState_, style);
    }
    return;
  }

  renderer_.drawText(fontId, static_cast<int>(x), static_cast<int>(y), buf, pixelState_, style);
}

// ===========================================================================
// v2.0.145 — image hooks.  Both methods use the caller-provided
// `resolveImagePath_` to translate the raw `<img src>` payload to an
// on-disk cached BMP path (LittleFS).  The legacy
// ChapterHtmlSlimParser.cacheImage path populates these BMPs upfront
// during chapter parse; the streaming render pipeline reuses them
// here without doing any decode work of its own.
// ===========================================================================

bool GfxRendererPaginatorAdapter::resolveImageSize(const uint8_t* path, size_t len,
                                                     uint16_t* outWidth, uint16_t* outHeight) {
  if (!resolveImagePath_ || path == nullptr || len == 0 ||
      outWidth == nullptr || outHeight == nullptr) {
    return false;
  }
  const std::string bmpPath = resolveImagePath_(path, len);
  if (bmpPath.empty()) return false;
  if (!LittleFS.exists(bmpPath.c_str())) return false;
  File bmpFile = LittleFS.open(bmpPath.c_str(), "r");
  if (!bmpFile) return false;
  Bitmap bm(bmpFile, /*dithering=*/false);
  const BmpReaderError err = bm.parseHeaders();
  if (err != BmpReaderError::Ok) {
    bmpFile.close();
    return false;
  }
  *outWidth = static_cast<uint16_t>(bm.getWidth());
  *outHeight = static_cast<uint16_t>(bm.getHeight());
  bmpFile.close();
  return true;
}

void GfxRendererPaginatorAdapter::drawImage(uint16_t x, uint16_t y, uint16_t width,
                                              uint16_t height, const uint8_t* path,
                                              size_t len) {
  if (!resolveImagePath_ || path == nullptr || len == 0) return;
  const std::string bmpPath = resolveImagePath_(path, len);
  if (bmpPath.empty()) return;
  if (!LittleFS.exists(bmpPath.c_str())) return;
  File bmpFile = LittleFS.open(bmpPath.c_str(), "r");
  if (!bmpFile) return;
  Bitmap bm(bmpFile, /*dithering=*/false);
  if (bm.parseHeaders() != BmpReaderError::Ok) {
    bmpFile.close();
    return;
  }
  // Use the resolved dimensions as the bounding box — Bitmap will
  // scale-to-fit within (width, height) without changing aspect.
  // GfxRenderer::drawBitmap takes (bitmap, x, y, maxWidth, maxHeight).
  // We cast away const because drawBitmap (non-const) wants a mutable
  // GfxRenderer; the adapter stores a const reference for the
  // measurement APIs that ARE const-correct, so we cast here for the
  // paint side.  Same pattern as the legacy Page::render path.
  const_cast<GfxRenderer&>(renderer_).drawBitmap(bm, static_cast<int>(x),
                                                   static_cast<int>(y),
                                                   static_cast<int>(width),
                                                   static_cast<int>(height));
  bmpFile.close();
}

}  // namespace snapix::smolport
