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
  RenderResult render(xtc::XtcParser& parser, uint32_t pageNum, std::function<void()> refreshCallback,
                      bool allowGrayscale = true);
  // True when the last successful render left the panel in grayscale mode
  // (the next refresh must then rebuild the B/W baseline — see ReaderState).
  bool lastRenderUsedGrayscale() const { return lastRenderUsedGrayscale_; }

 private:
  GfxRenderer& renderer_;
  bool lastRenderUsedGrayscale_ = false;

  // Render 1-bit B&W page (standard XTC)
  void render1Bit(const uint8_t* buffer, uint16_t width, uint16_t height);

  // Render 2-bit grayscale page (XTCH format)
  // Uses 4-pass rendering for e-ink grayscale display
  // Takes two separate plane buffers to handle heap fragmentation
  void render2BitGrayscale(const uint8_t* plane1, const uint8_t* plane2, uint16_t width, uint16_t height);
  // v3.11.12 — black & white rendering of a 2-bit page straight from the file
  // stream, no plane buffers (used when the heap cannot give 2 × 48 KB).
  // v3.11.12 — full grayscale with only plane 1 resident (48 KB); plane 2 is
  // re-streamed from the file for each pass.  Takes ownership of `plane1`.
  RenderResult renderGrayscaleOnePlane(xtc::XtcParser& parser, uint32_t pageNum, uint16_t width, uint16_t height,
                                       size_t planeSize, uint8_t* plane1,
                                       const std::function<void()>& refreshCallback);
  RenderResult renderStreaming2BitAsBw(xtc::XtcParser& parser, uint32_t pageNum, uint16_t width,
                                       uint16_t height, size_t planeSize, const std::function<void()>& refreshCallback);
};

}  // namespace snapix
