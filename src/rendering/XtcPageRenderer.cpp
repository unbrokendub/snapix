#include "XtcPageRenderer.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Xtc/XtcParser.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#define TAG "XTC_RENDER"

namespace snapix {

constexpr uint16_t MAX_PAGE_WIDTH = 2048;
constexpr uint16_t MAX_PAGE_HEIGHT = 2048;

XtcPageRenderer::XtcPageRenderer(GfxRenderer& renderer) : renderer_(renderer) {}

XtcPageRenderer::RenderResult XtcPageRenderer::render(xtc::XtcParser& parser, uint32_t pageNum,
                                                      std::function<void()> refreshCallback,
                                                      const bool allowGrayscale, const bool deferGray) {
  lastRenderUsedGrayscale_ = false;
  grayPending_ = false;
  // Bounds check
  if (pageNum >= parser.getPageCount()) {
    return RenderResult::EndOfBook;
  }

  const uint16_t pageWidth = parser.getWidth();
  const uint16_t pageHeight = parser.getHeight();
  const uint8_t bitDepth = parser.getBitDepth();

  if (pageWidth == 0 || pageHeight == 0 || pageWidth > MAX_PAGE_WIDTH || pageHeight > MAX_PAGE_HEIGHT) {
    LOG_ERR(TAG, "Invalid page dimensions");
    return RenderResult::InvalidDimensions;
  }

  // Calculate buffer size based on bit depth
  const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
  size_t bufferSize;
  uint8_t* plane1Buffer = nullptr;
  uint8_t* plane2Buffer = nullptr;

  if (bitDepth == 2 && !allowGrayscale) {
    return renderStreaming2BitAsBw(parser, pageNum, pageWidth, pageHeight, planeSize, refreshCallback);
  }

  if (bitDepth == 2 && deferGray) {
    // v3.11.12 — black & white now (the same base the grayscale path shows
    // first), gray passes on the caller's next idle loop step.
    const RenderResult base =
        renderStreaming2BitAsBw(parser, pageNum, pageWidth, pageHeight, planeSize, refreshCallback);
    if (base == RenderResult::Success) {
      grayPending_ = true;
      grayPendingPage_ = pageNum;
    }
    return base;
  }

  if (bitDepth == 2) {
    // Split allocation: allocate two separate buffers to handle heap fragmentation
    // Two 48KB blocks are easier to find than one 96KB contiguous block
    plane1Buffer = static_cast<uint8_t*>(malloc(planeSize));
    if (!plane1Buffer) {
      // v3.11.12 — not even one 48 KB block: show the page in black & white
      // straight from the stream instead of "Memory error".
      LOG_INF(TAG, "No 48 KB block for a grayscale plane (free=%lu largest=%lu) — rendering B/W",
              static_cast<unsigned long>(ESP.getFreeHeap()),
              static_cast<unsigned long>(ESP.getMaxAllocHeap()));
      return renderStreaming2BitAsBw(parser, pageNum, pageWidth, pageHeight, planeSize, refreshCallback);
    }
    plane2Buffer = static_cast<uint8_t*>(malloc(planeSize));
    if (!plane2Buffer) {
      // v3.11.12 — the reader typically has ~85 KB free, so the second 48 KB
      // plane rarely fits (pre-fix: "Memory error").  Keep plane 1 resident
      // and re-stream plane 2 from the file for each grayscale pass.
      return renderGrayscaleOnePlane(parser, pageNum, pageWidth, pageHeight, planeSize, plane1Buffer,
                                     refreshCallback);
    }

    bufferSize = planeSize * 2;
  } else {
    bufferSize = static_cast<size_t>((pageWidth + 7) / 8) * pageHeight;
    plane1Buffer = static_cast<uint8_t*>(malloc(bufferSize));
    if (!plane1Buffer) {
      LOG_ERR(TAG, "Failed to allocate buffer (%zu bytes, free heap: %lu)", bufferSize,
              static_cast<unsigned long>(ESP.getFreeHeap()));
      return RenderResult::AllocationFailed;
    }
  }

  // Load page data
  size_t bytesRead = 0;
  if (bitDepth == 2) {
    if (!loadPlanes(parser, pageNum, planeSize, plane1Buffer, plane2Buffer)) {
      LOG_ERR(TAG, "Failed to load page %u (streaming error)", pageNum);
      free(plane1Buffer);
      free(plane2Buffer);
      return RenderResult::PageLoadFailed;
    }
  } else {
    bytesRead = parser.loadPage(pageNum, plane1Buffer, bufferSize);
    if (bytesRead == 0) {
      LOG_ERR(TAG, "Failed to load page %u", pageNum);
      free(plane1Buffer);
      return RenderResult::PageLoadFailed;
    }
  }

  // Clear screen and render
  renderer_.clearScreen();

  if (bitDepth == 2) {
    render2BitGrayscale(plane1Buffer, plane2Buffer, pageWidth, pageHeight);
    refreshCallback();
    applyGrayPasses(plane1Buffer, plane2Buffer, pageWidth, pageHeight);
    LOG_DBG(TAG, "Rendered page %u/%u (2-bit grayscale)", pageNum + 1, parser.getPageCount());
    lastRenderUsedGrayscale_ = true;
    free(plane2Buffer);
  } else {
    render1Bit(plane1Buffer, pageWidth, pageHeight);
    refreshCallback();
    LOG_DBG(TAG, "Rendered page %u/%u (%u-bit)", pageNum + 1, parser.getPageCount(), bitDepth);
  }

  free(plane1Buffer);
  return RenderResult::Success;
}

bool XtcPageRenderer::loadPlanes(xtc::XtcParser& parser, const uint32_t pageNum, const size_t planeSize,
                                 uint8_t* plane1, uint8_t* plane2) {
  // Use streaming to load into separate buffers
  const xtc::XtcError err = parser.loadPageStreaming(
      pageNum,
      [&](const uint8_t* data, size_t size, size_t offset) {
        // Direct data to the appropriate buffer based on offset
        size_t remaining = size;
        size_t srcOffset = 0;

        while (remaining > 0) {
          size_t currentOffset = offset + srcOffset;
          if (currentOffset < planeSize) {
            // Writing to plane1
            size_t toWrite = std::min(remaining, planeSize - currentOffset);
            memcpy(plane1 + currentOffset, data + srcOffset, toWrite);
            srcOffset += toWrite;
            remaining -= toWrite;
          } else {
            // Writing to plane2
            size_t plane2Offset = currentOffset - planeSize;
            size_t toWrite = std::min(remaining, planeSize - plane2Offset);
            memcpy(plane2 + plane2Offset, data + srcOffset, toWrite);
            srcOffset += toWrite;
            remaining -= toWrite;
          }
        }
      },
      4096);
  return err == xtc::XtcError::OK;
}

void XtcPageRenderer::applyGrayPasses(const uint8_t* plane1, const uint8_t* plane2, const uint16_t pageWidth,
                                      const uint16_t pageHeight) {
  // Grayscale rendering requires additional passes
  const size_t colBytes = (pageHeight + 7) / 8;

  auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
    const size_t colIndex = pageWidth - 1 - x;
    const size_t byteInCol = y / 8;
    const size_t bitInByte = 7 - (y % 8);
    const size_t byteOffset = colIndex * colBytes + byteInCol;
    const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
    const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
    return (bit1 << 1) | bit2;
  };

  // Pass 2: LSB buffer - mark DARK gray (value 1)
  renderer_.clearScreen(0x00);
  for (uint16_t y = 0; y < pageHeight; y++) {
    for (uint16_t x = 0; x < pageWidth; x++) {
      if (getPixelValue(x, y) == 1) {
        renderer_.drawPixel(x, y, false);
      }
    }
    if (y % 100 == 0) esp_task_wdt_reset();
  }
  renderer_.copyGrayscaleLsbBuffers();

  // Pass 3: MSB buffer - mark LIGHT AND DARK gray (value 1 or 2)
  renderer_.clearScreen(0x00);
  for (uint16_t y = 0; y < pageHeight; y++) {
    for (uint16_t x = 0; x < pageWidth; x++) {
      const uint8_t pv = getPixelValue(x, y);
      if (pv == 1 || pv == 2) {
        renderer_.drawPixel(x, y, false);
      }
    }
    if (y % 100 == 0) esp_task_wdt_reset();
  }
  renderer_.copyGrayscaleMsbBuffers();

  renderer_.displayGrayBuffer();

  // Restore framebuffer for next page render WITHOUT hardware cleanup.
  // Leave inGrayscaleMode = true and LSB/MSB data in BW/RED RAM so that
  // displayBuffer()'s grayscaleRevert() can apply lut_grayscale_revert
  // to drive the panel from gray back to BW on the next page.
  renderer_.clearScreen();
  for (uint16_t y = 0; y < pageHeight; y++) {
    for (uint16_t x = 0; x < pageWidth; x++) {
      if (getPixelValue(x, y) >= 1) {
        renderer_.drawPixel(x, y, true);
      }
    }
    if (y % 100 == 0) esp_task_wdt_reset();
  }
}

XtcPageRenderer::RenderResult XtcPageRenderer::renderPendingGray(xtc::XtcParser& parser) {
  if (!grayPending_) return RenderResult::Success;
  grayPending_ = false;
  lastRenderUsedGrayscale_ = false;
  const uint32_t pageNum = grayPendingPage_;
  if (pageNum >= parser.getPageCount() || parser.getBitDepth() != 2) return RenderResult::Success;

  const uint16_t pageWidth = parser.getWidth();
  const uint16_t pageHeight = parser.getHeight();
  const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;

  uint8_t* plane1 = static_cast<uint8_t*>(malloc(planeSize));
  if (!plane1) {
    // The black & white base stays on screen — a complete, readable page.
    LOG_INF(TAG, "No 48 KB block for a grayscale plane (free=%lu largest=%lu) — page stays B/W",
            static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    return RenderResult::Success;
  }
  uint8_t* plane2 = static_cast<uint8_t*>(malloc(planeSize));
  if (!plane2) {
    return renderGrayscaleOnePlane(parser, pageNum, pageWidth, pageHeight, planeSize, plane1, nullptr,
                                   /*baseShown=*/true);
  }
  if (!loadPlanes(parser, pageNum, planeSize, plane1, plane2)) {
    LOG_ERR(TAG, "Failed to load page %u (deferred grayscale)", pageNum);
    free(plane1);
    free(plane2);
    return RenderResult::PageLoadFailed;
  }
  applyGrayPasses(plane1, plane2, pageWidth, pageHeight);
  free(plane2);
  free(plane1);
  lastRenderUsedGrayscale_ = true;
  LOG_DBG(TAG, "Rendered page %u/%u (2-bit grayscale, deferred passes)", pageNum + 1, parser.getPageCount());
  return RenderResult::Success;
}

void XtcPageRenderer::render1Bit(const uint8_t* buffer, uint16_t width, uint16_t height) {
  const size_t srcRowBytes = (width + 7) / 8;

  for (uint16_t srcY = 0; srcY < height; srcY++) {
    const size_t srcRowStart = srcY * srcRowBytes;

    for (size_t byteIdx = 0; byteIdx < srcRowBytes; byteIdx++) {
      const uint8_t byte = buffer[srcRowStart + byteIdx];

      // Fast path: all white (0xFF) - skip entirely
      if (byte == 0xFF) continue;

      const uint16_t baseX = byteIdx * 8;

      // Fast path: all black (0x00) - draw all 8 pixels
      if (byte == 0x00) {
        for (int bit = 0; bit < 8 && baseX + bit < width; bit++) {
          renderer_.drawPixel(baseX + bit, srcY, true);
        }
        continue;
      }

      // Mixed byte - process individual bits (MSB first, bit 7 = leftmost)
      for (int bit = 7; bit >= 0; bit--) {
        const uint16_t x = baseX + (7 - bit);
        if (x >= width) break;
        if (!((byte >> bit) & 1)) {  // XTC: 0 = black, 1 = white
          renderer_.drawPixel(x, srcY, true);
        }
      }
    }
  }
}

namespace {
// Stream one 2-bit page and call fn(x, y, value) for every pixel whose plane-2
// byte arrives, combining it with the resident plane 1 (copied from the same
// stream on the first pass).  XTCH layout: plane 1 then plane 2, each
// column-major, columns right to left, MSB = topmost pixel; value = bit1<<1|bit2.
template <typename Fn>
bool streamGrayPixels(xtc::XtcParser& parser, const uint32_t pageNum, const uint16_t width, const uint16_t height,
                      const size_t planeSize, uint8_t* plane1, bool& plane1Loaded, Fn&& fn) {
  const size_t colBytes = (height + 7) / 8;
  const bool copyPlane1 = !plane1Loaded;
  const xtc::XtcError err = parser.loadPageStreaming(
      pageNum,
      [&](const uint8_t* data, size_t size, size_t offset) {
        for (size_t i = 0; i < size; ++i) {
          const size_t o = offset + i;
          if (o < planeSize) {
            if (copyPlane1) plane1[o] = data[i];
            continue;
          }
          const size_t p = o - planeSize;
          if (p >= planeSize) continue;
          const size_t colIndex = p / colBytes;
          if (colIndex >= width) continue;
          const uint8_t b1 = plane1[p];
          const uint8_t b2 = data[i];
          const uint16_t x = static_cast<uint16_t>(width - 1 - colIndex);
          const size_t yBase = (p % colBytes) * 8;
          for (int b = 0; b < 8; ++b) {
            const size_t y = yBase + static_cast<size_t>(b);
            if (y >= height) break;
            const uint8_t v = static_cast<uint8_t>((((b1 >> (7 - b)) & 1) << 1) | ((b2 >> (7 - b)) & 1));
            fn(x, static_cast<uint16_t>(y), v);
          }
        }
        esp_task_wdt_reset();
      },
      4096);
  if (err != xtc::XtcError::OK) return false;
  plane1Loaded = true;
  return true;
}
}  // namespace

XtcPageRenderer::RenderResult XtcPageRenderer::renderGrayscaleOnePlane(xtc::XtcParser& parser, uint32_t pageNum,
                                                                      const uint16_t width, const uint16_t height,
                                                                      const size_t planeSize, uint8_t* plane1,
                                                                      const std::function<void()>& refreshCallback,
                                                                      const bool baseShown) {
  // Same four passes as the two-buffer path in render(), each re-reading the
  // page so plane 2 never needs its own 48 KB.
  bool loaded = false;
  auto fail = [&]() {
    LOG_ERR(TAG, "Failed to load page %u (one-plane grayscale)", pageNum);
    free(plane1);
    return RenderResult::PageLoadFailed;
  };

  // Pass 1: B/W base — every non-white pixel black.  Already on the panel
  // for deferred gray: pass 2 then loads plane 1 from its own stream.
  if (!baseShown) {
    renderer_.clearScreen();
    if (!streamGrayPixels(parser, pageNum, width, height, planeSize, plane1, loaded,
                          [&](uint16_t x, uint16_t y, uint8_t v) {
                            if (v >= 1) renderer_.drawPixel(x, y, true);
                          })) {
      return fail();
    }
    refreshCallback();
  }

  // Pass 2: LSB buffer — dark gray (value 1).
  renderer_.clearScreen(0x00);
  if (!streamGrayPixels(parser, pageNum, width, height, planeSize, plane1, loaded,
                        [&](uint16_t x, uint16_t y, uint8_t v) {
                          if (v == 1) renderer_.drawPixel(x, y, false);
                        })) {
    return fail();
  }
  renderer_.copyGrayscaleLsbBuffers();

  // Pass 3: MSB buffer — light and dark gray (value 1 or 2).
  renderer_.clearScreen(0x00);
  if (!streamGrayPixels(parser, pageNum, width, height, planeSize, plane1, loaded,
                        [&](uint16_t x, uint16_t y, uint8_t v) {
                          if (v == 1 || v == 2) renderer_.drawPixel(x, y, false);
                        })) {
    return fail();
  }
  renderer_.copyGrayscaleMsbBuffers();
  renderer_.displayGrayBuffer();

  // Restore the B/W framebuffer for the next page (see render()).
  renderer_.clearScreen();
  if (!streamGrayPixels(parser, pageNum, width, height, planeSize, plane1, loaded,
                        [&](uint16_t x, uint16_t y, uint8_t v) {
                          if (v >= 1) renderer_.drawPixel(x, y, true);
                        })) {
    return fail();
  }
  free(plane1);
  lastRenderUsedGrayscale_ = true;
  LOG_DBG(TAG, "Rendered page %u/%u (2-bit grayscale, one plane resident)", pageNum + 1, parser.getPageCount());
  return RenderResult::Success;
}

XtcPageRenderer::RenderResult XtcPageRenderer::renderStreaming2BitAsBw(xtc::XtcParser& parser, uint32_t pageNum,
                                                                      const uint16_t width, const uint16_t height,
                                                                      const size_t planeSize,
                                                                      const std::function<void()>& refreshCallback) {
  // Same image the grayscale path shows as its B/W base (pass 1 and the final
  // restore): a pixel is black when it is non-white in EITHER plane, so each
  // plane's bytes can be drawn as they stream in — plane 2 lands on the same
  // pixels as plane 1.  Column-major, columns right to left, MSB = topmost.
  const size_t colBytes = (height + 7) / 8;
  renderer_.clearScreen();
  const xtc::XtcError err = parser.loadPageStreaming(
      pageNum,
      [&](const uint8_t* data, size_t size, size_t offset) {
        for (size_t i = 0; i < size; ++i) {
          const uint8_t byte = data[i];
          if (byte == 0) continue;  // all 8 pixels white in this plane
          size_t o = offset + i;
          if (o >= planeSize) o -= planeSize;
          if (o >= planeSize) continue;
          const size_t colIndex = o / colBytes;
          if (colIndex >= width) continue;
          const uint16_t x = static_cast<uint16_t>(width - 1 - colIndex);
          const size_t yBase = (o % colBytes) * 8;
          for (int b = 0; b < 8; ++b) {
            const size_t y = yBase + static_cast<size_t>(b);
            if (y >= height) break;
            if ((byte >> (7 - b)) & 1) renderer_.drawPixel(x, static_cast<uint16_t>(y), true);
          }
        }
        esp_task_wdt_reset();
      },
      4096);
  if (err != xtc::XtcError::OK) {
    LOG_ERR(TAG, "Failed to load page %u (streaming B/W)", pageNum);
    return RenderResult::PageLoadFailed;
  }
  refreshCallback();
  LOG_DBG(TAG, "Rendered page %u/%u (2-bit as B/W, low memory)", pageNum + 1, parser.getPageCount());
  return RenderResult::Success;
}

void XtcPageRenderer::render2BitGrayscale(const uint8_t* plane1, const uint8_t* plane2, uint16_t width,
                                          uint16_t height) {
  // XTCH 2-bit mode: Two bit planes, column-major order
  // - Columns scanned right to left (x = width-1 down to 0)
  // - 8 vertical pixels per byte (MSB = topmost pixel in group)
  // - First plane: Bit1, Second plane: Bit2
  // - Pixel value = (bit1 << 1) | bit2
  // - Grayscale: 0=White, 1=Dark Grey, 2=Light Grey, 3=Black

  const size_t colBytes = (height + 7) / 8;

  // Pass 1: BW buffer - draw all non-white pixels as black
  for (uint16_t y = 0; y < height; y++) {
    for (uint16_t x = 0; x < width; x++) {
      const size_t colIndex = width - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      const uint8_t pixelValue = (bit1 << 1) | bit2;

      if (pixelValue >= 1) {
        renderer_.drawPixel(x, y, true);
      }
    }
  }
}

}  // namespace snapix
