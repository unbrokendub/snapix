#pragma once

// =============================================================================
// GfxRendererPaginatorAdapter — bridges the v3 PaginatorRenderer interface
// to the existing Snapix GfxRenderer.
//
// PURPOSE (Phase R3 of the v3 architectural refactor):
//   StreamingPaginator (R1) calls PaginatorRenderer hooks to measure +
//   draw words.  This adapter translates those hooks into GfxRenderer
//   `getTextAdvanceWidth` / `getSpaceWidth` / `drawText` calls, mapping
//   the v3 style-bit encoding (`kStyleBold` / `kStyleItalic` /
//   `kStyleHeading`) onto the legacy `EpdFontFamily::Style` enum +
//   font-id selection.
//
// WHY A SEPARATE CLASS (not direct calls in StreamingPaginator):
//   * Testability — paginator tests can use a `RecordingPaginatorRenderer`
//     that captures all calls (already used in StreamingPaginatorTest).
//   * Decoupling — paginator doesn't know about GfxRenderer / fontIds /
//     EpdFontFamily; it just calls measure/draw on whatever renderer
//     was injected.  Same paginator can drive a host-side renderer
//     for golden-image testing.
//   * Encoding shift — heading detection on the v3 side is "any heading
//     style bit", but legacy needs the heading fontId; this adapter
//     owns that translation.
//
// LIFETIME:
//   Adapter holds non-owning references to a GfxRenderer + a small set
//   of fontIds (body / heading).  Constructed per render pass on the
//   caller's stack — no heap allocation, no per-call setup cost.
//
// NULL-TERMINATION:
//   PaginatorRenderer hands raw byte ranges (`uint8_t* + len`).  GfxRenderer
//   wants null-terminated `const char*`.  Adapter copies into a small
//   stack buffer (256 bytes) per call.  Words longer than 255 bytes are
//   clamped — extremely rare in practice (longest German compound is ~80
//   bytes, longest URL is ~200 bytes).
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <EpdFontFamily.h>

#include "StreamingPaginator.h"

class GfxRenderer;

namespace snapix::smolport {

// v2.0.145 — image-resolver callback type.  Adapter calls this with the
// raw `<img src="...">` bytes from the marker stream; caller resolves
// to an on-disk BMP path (the existing chapter-cache convention).  An
// empty return string signals "image not available" — paginator skips
// the image.
using ResolveImagePathFn =
    std::function<std::string(const uint8_t* path, size_t len)>;

class GfxRendererPaginatorAdapter : public PaginatorRenderer {
 public:
  // `bodyFontId` and `headingFontId` come from the caller's font setup
  // (typically RenderConfig).  `pixelState` is forwarded as the legacy
  // `black` flag in drawText (true = render in black ink).
  //
  // v2.0.145 — `resolveImagePath` (optional, default empty) hooks the
  // existing ChapterHtmlSlimParser image cache so streaming renders
  // can paint inline images.  Pass `{}` (default) to disable image
  // rendering — paginator's onImageRef becomes a no-op.
  //
  // v2.0.146 — `fakeBoldMode` matches the legacy
  // `TextBlock::fakeBold` semantics: 0 = off (use real bold font),
  // 1 = bold (2× draw at x, x+1 with REGULAR), 2 = extrabold (3×
  // draw at x-1, x, x+1).  When non-zero, BOLD / BOLD_ITALIC styles
  // are drawn AND measured as REGULAR / ITALIC so the multi-pass
  // simulation matches the layout calculation.  Lets users use a
  // body-only font (no bold variant on disk) without losing visual
  // emphasis on `<b>` / `<strong>` / `<h>` content.
  GfxRendererPaginatorAdapter(const GfxRenderer& renderer, int bodyFontId, int headingFontId,
                              bool pixelState = true,
                              ResolveImagePathFn resolveImagePath = {},
                              uint8_t fakeBoldMode = 0,
                              int superSubFontId = 0);  // v3.6.0 — 0 = no shrink

  // PaginatorRenderer overrides
  uint16_t measureWidth(const uint8_t* text, size_t len, uint8_t styleBits) override;
  uint16_t getSpaceWidth(uint8_t styleBits) override;
  void drawWord(uint16_t x, uint16_t y, const uint8_t* text, size_t len, uint8_t styleBits) override;
  // v2.0.145 — image hooks (return false / no-op if no resolver wired).
  bool resolveImageSize(const uint8_t* path, size_t len, uint16_t* outWidth,
                          uint16_t* outHeight) override;
  void drawImage(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                  const uint8_t* path, size_t len) override;

 private:
  const GfxRenderer& renderer_;
  int bodyFontId_;
  int headingFontId_;
  int superSubFontId_ = 0;  // v3.6.0 — smaller font for <sup>/<sub> (0 = body)
  bool pixelState_;
  ResolveImagePathFn resolveImagePath_;
  uint8_t fakeBoldMode_ = 0;  // v2.0.146 — 0=off, 1=bold, 2=extrabold

  // v2.0.146 — apply fake-bold substitution to `*style`: BOLD →
  // REGULAR, BOLD_ITALIC → ITALIC, when fakeBoldMode_ > 0.  Returns
  // true if the substitution was applied (caller must do the
  // multi-pass draw at draw time).
  bool applyFakeBoldSubstitution(EpdFontFamily::Style* style) const;

  // Stack-only scratch for null-termination of word bytes.  256 bytes
  // covers any realistic word; longer runs are clamped with a debug
  // log (silent in non-debug builds).
  static constexpr size_t kWordBufBytes = 256;

  // Decode style bits → (fontId, EpdFontFamily::Style) pair.
  void resolveStyle(uint8_t styleBits, int* outFontId, EpdFontFamily::Style* outStyle) const;

  // Copy `text[0..len)` into `outBuf` and append `\0`.  Clamps to
  // `outBufSize - 1` bytes payload.  Returns the byte length written
  // (excluding the null terminator).
  static size_t copyTermBytes(const uint8_t* text, size_t len, char* outBuf, size_t outBufSize);
};

}  // namespace snapix::smolport
