#pragma once

#include <cstdint>
#include <functional>

class GfxRenderer;

namespace xtc {
class XtcParser;
}

namespace snapix {

// XtcPageRenderer - Renders XTC/XTCH binary page data to GfxRenderer
// Supports 1-bit (B&W) and 2-bit (4-level grayscale) formats
class XtcPageRenderer {
 public:
  // Result of render operation
  enum class RenderResult { Success, EndOfBook, InvalidDimensions, AllocationFailed, PageLoadFailed };

  explicit XtcPageRenderer(GfxRenderer& renderer);

  // Render a page from the parser
  // refreshCallback is called when display refresh is needed (for pagesUntilFullRefresh logic)
  // v3.11.12 — `allowGrayscale` = the reader's anti-aliasing setting: when off,
  // 2-bit (XTCH) pages are drawn black & white in one refresh, skipping the
  // gray LUT passes entirely.
  //
  // v3.11.12 — `deferGray`: a 2-bit page is shown black & white (one refresh,
  // streamed, no plane buffers) and its gray passes are left owed; the caller
  // applies them with renderPendingGray() on a later UI-loop step unless the
  // reader moved on (cancelPendingGray()).  Each step then blocks the loop for
  // one panel refresh instead of two, and a page turn skips the gray step.
  RenderResult render(xtc::XtcParser& parser, uint32_t pageNum, std::function<void()> refreshCallback,
                      bool allowGrayscale = true, bool deferGray = false);
  // True when the last successful render left the panel in grayscale mode
  // (the next refresh must then rebuild the B/W baseline — see ReaderState).
  bool lastRenderUsedGrayscale() const { return lastRenderUsedGrayscale_; }

  // v3.11.12 — deferred gray passes (see `deferGray`).  renderPendingGray()
  // expects the framebuffer and panel to still hold the black & white base
  // drawn by render(); without a 48 KB block it leaves the page black & white.
  bool grayPending() const { return grayPending_; }
  void cancelPendingGray() { grayPending_ = false; }
  RenderResult renderPendingGray(xtc::XtcParser& parser);

 private:
  GfxRenderer& renderer_;
  bool lastRenderUsedGrayscale_ = false;
  bool grayPending_ = false;
  uint32_t grayPendingPage_ = 0;

  // Render 1-bit B&W page (standard XTC)
  void render1Bit(const uint8_t* buffer, uint16_t width, uint16_t height);

  // Render 2-bit grayscale page (XTCH format)
  // Uses 4-pass rendering for e-ink grayscale display
  // Takes two separate plane buffers to handle heap fragmentation
  void render2BitGrayscale(const uint8_t* plane1, const uint8_t* plane2, uint16_t width, uint16_t height);
  // Gray passes 2-4 over a shown B/W base: LSB + MSB planes, gray refresh,
  // then the B/W framebuffer restored for the next page's refresh.
  void applyGrayPasses(const uint8_t* plane1, const uint8_t* plane2, uint16_t width, uint16_t height);
  // Streams the page into the two plane buffers.
  bool loadPlanes(xtc::XtcParser& parser, uint32_t pageNum, size_t planeSize, uint8_t* plane1, uint8_t* plane2);
  // v3.11.12 — black & white rendering of a 2-bit page straight from the file
  // stream, no plane buffers (used when the heap cannot give 2 × 48 KB).
  // v3.11.12 — full grayscale with only plane 1 resident (48 KB); plane 2 is
  // re-streamed from the file for each pass.  Takes ownership of `plane1`.
  // `baseShown`: the B/W base is already on the panel (deferred gray) — skip pass 1.
  RenderResult renderGrayscaleOnePlane(xtc::XtcParser& parser, uint32_t pageNum, uint16_t width, uint16_t height,
                                       size_t planeSize, uint8_t* plane1,
                                       const std::function<void()>& refreshCallback, bool baseShown = false);
  RenderResult renderStreaming2BitAsBw(xtc::XtcParser& parser, uint32_t pageNum, uint16_t width,
                                       uint16_t height, size_t planeSize, const std::function<void()>& refreshCallback);
};

}  // namespace snapix
