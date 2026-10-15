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
                                                          uint8_t fakeBoldMode, int superSubFontId)
    : renderer_(renderer),
      bodyFontId_(bodyFontId),
      headingFontId_(headingFontId),
      superSubFontId_(superSubFontId),
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
  // v3.6.0 — super/subscript use the smaller built-in font (wins over heading
  // too, e.g. a footnote ref inside a heading).  Falls back to the body font
  // when no small font was configured.
  if ((styleBits & (kStyleSuper | kStyleSub)) && superSubFontId_ != 0) {
    *outFontId = superSubFontId_;
  } else if (styleBits & kStyleHeading) {
    // Heading wins over inline styles for fontId selection — same convention
    // as the legacy ChapterHtmlSlimParser layout.
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

  // v3.6.0 — super/subscript vertical placement.  drawText's y is the glyph
  // top; the body baseline sits at y + ascender(body).  Superscript: draw the
  // small font at the line top (offset 0) so it rides above the body baseline.
  // Subscript: shift down so the small glyph's baseline aligns with the body
  // baseline (offset = ascender(body) − ascender(small)), placing it low.
  int yAdj = 0;
  if (styleBits & kStyleSub) {
    const int bodyAsc = renderer_.getFontAscenderSize(bodyFontId_);
    const int smallAsc = renderer_.getFontAscenderSize(fontId);
    if (bodyAsc > smallAsc) yAdj = bodyAsc - smallAsc;
  }
  const int yy = static_cast<int>(y) + yAdj;

  // v2.0.146 — fake-bold multi-pass.  Mirrors the legacy
  // TextBlock::render path: when fakeBold is on and the requested
  // style is BOLD/BOLD_ITALIC, substitute REGULAR/ITALIC and
  // draw 2× (mode=1, at x and x+1) or 3× (mode=2, at x-1, x,
  // x+1).  All passes use the same pixelState_/fontId so the
  // visual outcome is a thicker version of the regular glyph.
  const bool fakeBold = applyFakeBoldSubstitution(&style);
  if (fakeBold) {
    const int xi = static_cast<int>(x);
    if (fakeBoldMode_ == 2) {
      renderer_.drawText(fontId, xi - 1, yy, buf, pixelState_, style);
      renderer_.drawText(fontId, xi,     yy, buf, pixelState_, style);
      renderer_.drawText(fontId, xi + 1, yy, buf, pixelState_, style);
    } else {  // mode 1
      renderer_.drawText(fontId, xi,     yy, buf, pixelState_, style);
      renderer_.drawText(fontId, xi + 1, yy, buf, pixelState_, style);
    }
    return;
  }

  renderer_.drawText(fontId, static_cast<int>(x), yy, buf, pixelState_, style);
}

// ===========================================================================
// v2.0.145 — image hooks.  Both methods use the caller-provided
// `resolveImagePath_` to translate the raw `<img src>` payload to an
// on-disk cached BMP path (LittleFS).  The legacy
// ChapterHtmlSlimParser.cacheImage path populates these BMPs upfront
// during chapter parse; the streaming render pipeline reuses them
// here without doing any decode work of its own.
// ===========================================================================

namespace {

// v3.11.12 — "<bmp>.dim" sidecar: the image's LAYOUT size, recorded once and
// authoritative from then on.  8 bytes: 'D','1', w LE16, h LE16, 2 spare.
// w == h == 0 records "image unusable — lay out as absent".
bool readDimSidecar(const std::string& dimPath, uint16_t& w, uint16_t& h) {
  if (!LittleFS.exists(dimPath.c_str())) return false;
  File f = LittleFS.open(dimPath.c_str(), "r");
  if (!f) return false;
  uint8_t b[8];
  const size_t got = f.read(b, sizeof(b));
  f.close();
  if (got != sizeof(b) || b[0] != 'D' || b[1] != '1') return false;
  w = static_cast<uint16_t>(b[2] | (b[3] << 8));
  h = static_cast<uint16_t>(b[4] | (b[5] << 8));
  return true;
}

void writeDimSidecar(const std::string& dimPath, uint16_t w, uint16_t h) {
  // littlefs commits a file's contents atomically on close (copy-on-write), so
  // a reset mid-write leaves no file or the old one — never a torn sidecar;
  // readDimSidecar also checks magic + length.  No temp-file + rename (one
  // metadata commit instead of three per image).
  File f = LittleFS.open(dimPath.c_str(), "w");
  if (!f) {
    // Image cache dir not created yet (nothing decoded for this book so far).
    const size_t slash = dimPath.rfind('/');
    if (slash == std::string::npos || slash == 0) return;
    LittleFS.mkdir(dimPath.substr(0, slash).c_str());
    f = LittleFS.open(dimPath.c_str(), "w");
    if (!f) return;
  }
  const uint8_t b[8] = {'D', '1', static_cast<uint8_t>(w & 0xFF), static_cast<uint8_t>(w >> 8),
                        static_cast<uint8_t>(h & 0xFF), static_cast<uint8_t>(h >> 8), 0, 0};
  const bool ok = f.write(b, sizeof(b)) == sizeof(b);
  f.close();
  if (!ok) LittleFS.remove(dimPath.c_str());
}

bool readBmpSize(const std::string& bmpPath, uint16_t& w, uint16_t& h) {
  if (!LittleFS.exists(bmpPath.c_str())) return false;
  File bmpFile = LittleFS.open(bmpPath.c_str(), "r");
  if (!bmpFile) return false;
  Bitmap bm(bmpFile, /*dithering=*/false);
  const bool ok = bm.parseHeaders() == BmpReaderError::Ok && bm.getWidth() > 0 && bm.getHeight() > 0;
  if (ok) {
    w = static_cast<uint16_t>(bm.getWidth());
    h = static_cast<uint16_t>(bm.getHeight());
  }
  bmpFile.close();
  return ok;
}

}  // namespace

// v3.11.12 — the layout size of an image must be the SAME for the MEASURE walk
// that builds the page index and for every later render.  Pre-fix it was the
// cached BMP's header size, but the BMP is decoded asynchronously AFTER the
// index walk (EPUB: only once the page is first shown; FB2: BG queue), so the
// walk laid the image out as absent while the render placed it at full height
// → the page overflowed and the text after the image was never shown (and the
// stale index was persisted).  Now: the "<bmp>.dim" sidecar decides; when it
// is missing the size comes from the source header (probe), falling back to
// the BMP header, and is recorded so every later walk/render agrees.
bool GfxRendererPaginatorAdapter::resolveImageSize(const uint8_t* path, size_t len,
                                                     uint16_t* outWidth, uint16_t* outHeight) {
  if (!resolveImagePath_ || path == nullptr || len == 0 ||
      outWidth == nullptr || outHeight == nullptr) {
    return false;
  }
  const std::string bmpPath = resolveImagePath_(path, len);
  if (bmpPath.empty()) return false;
  const std::string dimPath = bmpPath + ".dim";

  uint16_t w = 0, h = 0;
  if (!readDimSidecar(dimPath, w, h)) {
    int probed = -1;
    if (probeImageSize_) probed = probeImageSize_(path, len, w, h);
    if (probed < 0) {
      if (!readBmpSize(bmpPath, w, h)) return false;  // unknown yet — nothing recorded
    } else if (probed == 0) {
      w = h = 0;
    }
    writeDimSidecar(dimPath, w, h);
  }
  if (w == 0 || h == 0) return false;
  *outWidth = w;
  *outHeight = h;
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
