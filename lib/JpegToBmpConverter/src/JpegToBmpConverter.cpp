#include "JpegToBmpConverter.h"

#include <Arduino.h>
#include <Logging.h>

#define TAG "JPEG"
#include <SdFat.h>
#include <SharedSpiLock.h>
#include <picojpeg.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "BitmapHelpers.h"

// Context structure for picojpeg callback
struct JpegReadContext {
  FsFile& file;
  // 4 KB pump buffer (heap-allocated to avoid ~4 KB stack burst — BG
  // worker stacks are 16 KB and picojpeg's call chain is moderately deep).
  // picojpeg's callback signature only allows 8-bit request sizes
  // (≤255 bytes), but we stage a much larger chunk in this buffer so we
  // acquire SharedBusLock + hit SDFat ~8× less often.  On a 150 KB JPEG
  // that's 37 SD reads instead of 300 — every avoided lock-acquire shaves
  // 20-50 ms of contention wait when the BG cache worker is also writing.
  // 4 KB matches the SDFat block size, so each read fills exactly one
  // cache line.
  static constexpr size_t kBufferSize = 4096;
  uint8_t* buffer;
  size_t bufferPos;
  size_t bufferFilled;
};

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;  // true: 8-bit grayscale (no quantization), false: 2-bit (4 levels)
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;     // true: scale image to target size before dithering
constexpr int TARGET_MAX_WIDTH = 450;   // Max width for cover images (0.6 aspect ratio to avoid scaling artifacts)
constexpr int TARGET_MAX_HEIGHT = 750;  // Max height for cover images (0.6 aspect ratio to avoid scaling artifacts)
// ============================================================================

inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 1-bit color depth (black and white)
static void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bit per pixel, round up to 4-byte boundary
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;  // 14 (file header) + 40 (DIB header) + 8 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 62);        // Offset to pixel data (14 + 40 + 8)

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 1);              // Bits per pixel (1 bit)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 2);     // colorsUsed
  write32(bmpOut, 2);     // colorsImportant

  // Color Palette (2 colors x 4 bytes = 8 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // Note: In 1-bit BMP, palette index 0 = black, 1 = white
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0xFF, 0xFF, 0xFF, 0x00   // Color 1: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Helper function: Write BMP header with 2-bit color depth
static void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 70);        // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 2);              // Bits per pixel (2 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 4);     // colorsUsed
  write32(bmpOut, 4);     // colorsImportant

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0x55, 0x55, 0x55, 0x00,  // Color 1: Dark gray (85)
      0xAA, 0xAA, 0xAA, 0x00,  // Color 2: Light gray (170)
      0xFF, 0xFF, 0xFF, 0x00   // Color 3: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// JPEG SOF markers - detect unsupported encoding types
// SOF0 (0xC0) = Baseline DCT (supported)
// SOF1 (0xC1) = Extended sequential DCT (supported)
// SOF2 (0xC2) = Progressive DCT (NOT supported)
// SOF9 (0xC9) = Extended sequential DCT, arithmetic (NOT supported)
// SOF10 (0xCA) = Progressive DCT, arithmetic (NOT supported)
static bool isUnsupportedJpeg(FsFile& file) {
  snapix::spi::SharedBusLock busLock;
  if (!busLock) {
    return true;
  }

  const uint64_t originalPos = file.position();
  file.seek(0);

  uint8_t buf[2];
  bool isProgressive = false;

  // Scan for SOF marker
  while (file.read(buf, 1) == 1) {
    if (buf[0] != 0xFF) continue;

    if (file.read(buf, 1) != 1) break;

    // Skip padding FFs
    while (buf[0] == 0xFF) {
      if (file.read(buf, 1) != 1) break;
    }

    const uint8_t marker = buf[0];

    // Check for unsupported SOF markers (progressive or arithmetic coding)
    if (marker == 0xC2 || marker == 0xC9 || marker == 0xCA) {
      isProgressive = true;
      break;
    }

    // Baseline/extended sequential DCT - supported
    if (marker == 0xC0 || marker == 0xC1) {
      isProgressive = false;
      break;
    }

    // Skip variable-length segments (not SOF, SOS, or standalone markers)
    if (marker != 0xD8 && marker != 0xD9 && marker != 0x01 && !(marker >= 0xD0 && marker <= 0xD7)) {
      if (file.read(buf, 2) != 2) break;
      const uint16_t len = (buf[0] << 8) | buf[1];
      if (len < 2) break;
      file.seek(file.position() + len - 2);
    }
  }

  file.seek(originalPos);
  return isProgressive;
}

// Callback function for picojpeg to read JPEG data
unsigned char JpegToBmpConverter::jpegReadCallback(unsigned char* pBuf, const unsigned char buf_size,
                                                   unsigned char* pBytes_actually_read, void* pCallback_data) {
  auto* context = static_cast<JpegReadContext*>(pCallback_data);

  if (!context || !context->file) {
    return PJPG_STREAM_READ_ERROR;
  }

  // Check if we need to refill our context buffer
  if (context->bufferPos >= context->bufferFilled) {
    snapix::spi::SharedBusLock busLock;
    if (!busLock) {
      return PJPG_STREAM_READ_ERROR;
    }
    const int readResult = context->file.read(context->buffer, JpegReadContext::kBufferSize);
    context->bufferPos = 0;

    if (readResult < 0) {
      *pBytes_actually_read = 0;
      return PJPG_STREAM_READ_ERROR;
    }

    context->bufferFilled = static_cast<size_t>(readResult);
    if (context->bufferFilled == 0) {
      // EOF or error
      *pBytes_actually_read = 0;
      return 0;  // Success (EOF is normal)
    }
  }

  // Copy available bytes to picojpeg's buffer
  const size_t available = context->bufferFilled - context->bufferPos;
  const size_t toRead = available < buf_size ? available : buf_size;

  memcpy(pBuf, context->buffer + context->bufferPos, toRead);
  context->bufferPos += toRead;
  *pBytes_actually_read = static_cast<unsigned char>(toRead);

  return 0;  // Success
}

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                     bool oneBit, bool quickMode,
                                                     const std::function<bool()>& shouldAbort) {
  LOG_INF(TAG, "Converting JPEG to %s BMP (target: %dx%d)%s", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight,
          quickMode ? " [QUICK]" : "");

  // Check for unsupported JPEG encoding (progressive or arithmetic) before attempting decode
  if (isUnsupportedJpeg(jpegFile)) {
    LOG_ERR(TAG, "Unsupported JPEG encoding (progressive or arithmetic), skipping");
    return false;
  }

  // Setup context for picojpeg callback.  4 KB pump buffer goes on heap
  // (RAII via std::unique_ptr) to keep the BG worker's 16 KB stack from
  // getting tight when picojpeg's call chain piles up on top.
  std::unique_ptr<uint8_t[]> jpegBuf(new (std::nothrow) uint8_t[JpegReadContext::kBufferSize]);
  if (!jpegBuf) {
    LOG_ERR(TAG, "JPEG decoder OOM allocating %u-byte read buffer",
            static_cast<unsigned>(JpegReadContext::kBufferSize));
    return false;
  }
  JpegReadContext context = {.file = jpegFile, .buffer = jpegBuf.get(), .bufferPos = 0, .bufferFilled = 0};

  // Initialize picojpeg decoder
  pjpeg_image_info_t imageInfo;
  const unsigned char status = pjpeg_decode_init(&imageInfo, jpegReadCallback, &context, 0);
  if (status != 0) {
    LOG_ERR(TAG, "JPEG decode init failed with error code: %d", status);
    return false;
  }

  LOG_INF(TAG, "JPEG dimensions: %dx%d, components: %d, MCUs: %dx%d", imageInfo.m_width, imageInfo.m_height,
          imageInfo.m_comps, imageInfo.m_MCUSPerRow, imageInfo.m_MCUSPerCol);

  // Safety limits to prevent memory issues on ESP32
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  constexpr int MAX_MCU_ROW_BYTES = 65536;

  if (imageInfo.m_width > MAX_IMAGE_WIDTH || imageInfo.m_height > MAX_IMAGE_HEIGHT) {
    LOG_ERR(TAG, "Image too large (%dx%d), max supported: %dx%d", imageInfo.m_width, imageInfo.m_height,
            MAX_IMAGE_WIDTH, MAX_IMAGE_HEIGHT);
    return false;
  }
  // Defensive: a malformed JPEG can still report 0×0 / 0-dimension MCUs after
  // a "successful" pjpeg_decode_init.  Reject before any divide-by-zero or
  // zero-byte allocation downstream.
  if (imageInfo.m_width == 0 || imageInfo.m_height == 0 || imageInfo.m_MCUWidth == 0 ||
      imageInfo.m_MCUHeight == 0 || imageInfo.m_MCUSPerRow == 0 || imageInfo.m_MCUSPerCol == 0) {
    LOG_ERR(TAG, "Malformed JPEG: zero dimension(s) %dx%d MCU=%dx%d MCUs=%dx%d", imageInfo.m_width,
            imageInfo.m_height, imageInfo.m_MCUWidth, imageInfo.m_MCUHeight, imageInfo.m_MCUSPerRow,
            imageInfo.m_MCUSPerCol);
    return false;
  }

  // Calculate output dimensions (pre-scale to fit display exactly)
  int outWidth = imageInfo.m_width;
  int outHeight = imageInfo.m_height;
  // Use fixed-point scaling (16.16) for sub-pixel accuracy
  uint32_t scaleX_fp = 65536;  // 1.0 in 16.16 fixed point
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (targetWidth > 0 && targetHeight > 0 && (imageInfo.m_width > targetWidth || imageInfo.m_height > targetHeight)) {
    // Calculate scale to fit within target dimensions while maintaining aspect ratio
    const float scaleToFitWidth = static_cast<float>(targetWidth) / imageInfo.m_width;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / imageInfo.m_height;
    // Choose smaller scale factor to ensure image fits within target dimensions (contain mode)
    const float scale = std::min(scaleToFitWidth, scaleToFitHeight);

    outWidth = static_cast<int>(imageInfo.m_width * scale);
    outHeight = static_cast<int>(imageInfo.m_height * scale);

    // Ensure at least 1 pixel
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    // Calculate fixed-point scale factors (source pixels per output pixel)
    // scaleX_fp = (srcWidth << 16) / outWidth
    scaleX_fp = (static_cast<uint32_t>(imageInfo.m_width) << 16) / outWidth;
    scaleY_fp = (static_cast<uint32_t>(imageInfo.m_height) << 16) / outHeight;
    needsScaling = true;

    LOG_INF(TAG, "Pre-scaling %dx%d -> %dx%d (fit to %dx%d)", imageInfo.m_width, imageInfo.m_height, outWidth,
            outHeight, targetWidth, targetHeight);
  }

  // Write BMP header with output dimensions
  int bytesPerRow;
  {
    snapix::spi::SharedBusLock busLock;
    if (!busLock) {
      LOG_ERR(TAG, "Shared SPI lock unavailable for BMP header write");
      return false;
    }
    if (USE_8BIT_OUTPUT && !oneBit) {
      writeBmpHeader8bit(bmpOut, outWidth, outHeight);
      bytesPerRow = (outWidth + 3) / 4 * 4;
    } else if (oneBit) {
      writeBmpHeader1bit(bmpOut, outWidth, outHeight);
      bytesPerRow = (outWidth + 31) / 32 * 4;  // 1 bit per pixel
    } else {
      writeBmpHeader2bit(bmpOut, outWidth, outHeight);
      bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
    }
  }

  // Allocate row buffer
  auto* rowBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuffer) {
    LOG_ERR(TAG, "Failed to allocate row buffer");
    return false;
  }

  // Allocate a buffer for one MCU row worth of grayscale pixels
  // This is the minimal memory needed for streaming conversion
  const int mcuPixelHeight = imageInfo.m_MCUHeight;
  const int mcuRowPixels = imageInfo.m_width * mcuPixelHeight;

  // Validate MCU row buffer size before allocation
  if (mcuRowPixels > MAX_MCU_ROW_BYTES) {
    LOG_ERR(TAG, "MCU row buffer too large (%d bytes), max: %d", mcuRowPixels, MAX_MCU_ROW_BYTES);
    free(rowBuffer);
    return false;
  }

  auto* mcuRowBuffer = static_cast<uint8_t*>(malloc(mcuRowPixels));
  if (!mcuRowBuffer) {
    LOG_ERR(TAG, "Failed to allocate MCU row buffer (%d bytes)", mcuRowPixels);
    free(rowBuffer);
    return false;
  }

  // Create ditherer if enabled (skip in quickMode for faster preview)
  // Use OUTPUT dimensions for dithering (after prescaling)
  AtkinsonDitherer* atkinsonDitherer = nullptr;
  FloydSteinbergDitherer* fsDitherer = nullptr;
  Atkinson1BitDitherer* atkinson1BitDitherer = nullptr;

  if (!quickMode) {
    if (oneBit) {
      // For 1-bit output, use Atkinson dithering for better quality
      atkinson1BitDitherer = new (std::nothrow) Atkinson1BitDitherer(outWidth);
      if (!atkinson1BitDitherer) {
        LOG_ERR(TAG, "Failed to allocate 1-bit ditherer");
        free(mcuRowBuffer);
        free(rowBuffer);
        return false;
      }
    } else if (!USE_8BIT_OUTPUT) {
      if (USE_ATKINSON) {
        atkinsonDitherer = new (std::nothrow) AtkinsonDitherer(outWidth);
        if (!atkinsonDitherer) {
          LOG_ERR(TAG, "Failed to allocate Atkinson ditherer");
          free(mcuRowBuffer);
          free(rowBuffer);
          return false;
        }
      } else if (USE_FLOYD_STEINBERG) {
        fsDitherer = new (std::nothrow) FloydSteinbergDitherer(outWidth);
        if (!fsDitherer) {
          LOG_ERR(TAG, "Failed to allocate Floyd-Steinberg ditherer");
          free(mcuRowBuffer);
          free(rowBuffer);
          return false;
        }
      }
    }
  }

  // For scaling: accumulate source rows into scaled output rows
  // We need to track which source Y maps to which output Y
  // Using fixed-point: srcY_fp = outY * scaleY_fp (gives source Y in 16.16 format)
  uint32_t* rowAccum = nullptr;    // Accumulator for each output X (32-bit for larger sums)
  uint16_t* rowCount = nullptr;    // Count of source pixels accumulated per output X
  int currentOutY = 0;             // Current output row being accumulated
  uint32_t nextOutY_srcStart = 0;  // Source Y where next output row starts (16.16 fixed point)

  if (needsScaling) {
    rowAccum = new (std::nothrow) uint32_t[outWidth]();
    if (!rowAccum) {
      LOG_ERR(TAG, "Failed to allocate rowAccum (%d entries)", outWidth);
      if (atkinsonDitherer) delete atkinsonDitherer;
      if (fsDitherer) delete fsDitherer;
      if (atkinson1BitDitherer) delete atkinson1BitDitherer;
      free(mcuRowBuffer);
      free(rowBuffer);
      return false;
    }
    rowCount = new (std::nothrow) uint16_t[outWidth]();
    if (!rowCount) {
      LOG_ERR(TAG, "Failed to allocate rowCount (%d entries)", outWidth);
      delete[] rowAccum;
      if (atkinsonDitherer) delete atkinsonDitherer;
      if (fsDitherer) delete fsDitherer;
      if (atkinson1BitDitherer) delete atkinson1BitDitherer;
      free(mcuRowBuffer);
      free(rowBuffer);
      return false;
    }
    nextOutY_srcStart = scaleY_fp;  // First boundary is at scaleY_fp (source Y for outY=1)
  }

  // Process MCUs row-by-row and write to BMP as we go (top-down)
  const int mcuPixelWidth = imageInfo.m_MCUWidth;

  for (int mcuY = 0; mcuY < imageInfo.m_MCUSPerCol; mcuY++) {
    if (shouldAbort && shouldAbort()) {
      LOG_INF(TAG, "Abort requested during JPEG conversion");
      if (rowAccum) delete[] rowAccum;
      if (rowCount) delete[] rowCount;
      if (atkinsonDitherer) delete atkinsonDitherer;
      if (fsDitherer) delete fsDitherer;
      if (atkinson1BitDitherer) delete atkinson1BitDitherer;
      free(mcuRowBuffer);
      free(rowBuffer);
      return false;
    }

    if ((mcuY & 0x3) == 0) {
      // Yield cooperatively rather than delay — vTaskDelay() puts the task in
      // BLOCKED state which triggers FreeRTOS priority disinherit.  On
      // single-core ESP32-C3 this asserts when a recursive mutex is held
      // (SharedBusLock depth > 1).  taskYIELD() stays READY — no priority
      // inheritance bookkeeping.
      taskYIELD();
    }

    // Clear the MCU row buffer
    memset(mcuRowBuffer, 0, mcuRowPixels);

    // Decode one row of MCUs
    for (int mcuX = 0; mcuX < imageInfo.m_MCUSPerRow; mcuX++) {
      // Wide images (e.g. 50+ MCUs/row) can decode for 100ms+ between outer
      // mcuY abort checks.  Sample abort every 4 MCUs so a button-press preempt
      // is honoured within ~30ms instead of waiting for the row to finish.
      if ((mcuX & 0x3) == 0 && shouldAbort && shouldAbort()) {
        LOG_INF(TAG, "Abort requested mid-row during JPEG conversion (mcuY=%d mcuX=%d)", mcuY, mcuX);
        if (rowAccum) delete[] rowAccum;
        if (rowCount) delete[] rowCount;
        if (atkinsonDitherer) delete atkinsonDitherer;
        if (fsDitherer) delete fsDitherer;
        if (atkinson1BitDitherer) delete atkinson1BitDitherer;
        free(mcuRowBuffer);
        free(rowBuffer);
        return false;
      }
      const unsigned char mcuStatus = pjpeg_decode_mcu();
      if (mcuStatus != 0) {
        if (mcuStatus == PJPG_NO_MORE_BLOCKS) {
          LOG_ERR(TAG, "Unexpected end of blocks at MCU (%d, %d)", mcuX, mcuY);
        } else {
          LOG_ERR(TAG, "JPEG decode MCU failed at (%d, %d) with error code: %d", mcuX, mcuY, mcuStatus);
        }
        if (rowAccum) delete[] rowAccum;
        if (rowCount) delete[] rowCount;
        if (atkinsonDitherer) delete atkinsonDitherer;
        if (fsDitherer) delete fsDitherer;
        if (atkinson1BitDitherer) delete atkinson1BitDitherer;
        free(mcuRowBuffer);
        free(rowBuffer);
        return false;
      }

      // picojpeg stores MCU data in 8x8 blocks
      // Block layout: H2V2(16x16)=0,64,128,192 H2V1(16x8)=0,64 H1V2(8x16)=0,128
      for (int blockY = 0; blockY < mcuPixelHeight; blockY++) {
        for (int blockX = 0; blockX < mcuPixelWidth; blockX++) {
          const int pixelX = mcuX * mcuPixelWidth + blockX;
          if (pixelX >= imageInfo.m_width) continue;

          // Calculate proper block offset for picojpeg buffer.
          const int blockCol = blockX >> 3;
          const int blockRow = blockY >> 3;
          const int localX = blockX & 7;
          const int localY = blockY & 7;
          const int blocksPerRow = mcuPixelWidth >> 3;
          const int blockIndex = blockRow * blocksPerRow + blockCol;
          const int pixelOffset = blockIndex * 64 + localY * 8 + localX;

          uint8_t gray;
          if (imageInfo.m_comps == 1) {
            gray = imageInfo.m_pMCUBufR[pixelOffset];
          } else {
            const uint8_t r = imageInfo.m_pMCUBufR[pixelOffset];
            const uint8_t g = imageInfo.m_pMCUBufG[pixelOffset];
            const uint8_t b = imageInfo.m_pMCUBufB[pixelOffset];
            gray = rgbToGray(r, g, b);
          }

          mcuRowBuffer[blockY * imageInfo.m_width + pixelX] = gray;
        }
      }
    }

    // Process source rows from this MCU row
    const int startRow = mcuY * mcuPixelHeight;
    const int endRow = (mcuY + 1) * mcuPixelHeight;

    for (int y = startRow; y < endRow && y < imageInfo.m_height; y++) {
      const int bufferY = y - startRow;

      if (!needsScaling) {
        // No scaling - direct output (1:1 mapping)
        memset(rowBuffer, 0, bytesPerRow);

        if (USE_8BIT_OUTPUT && !oneBit) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = mcuRowBuffer[bufferY * imageInfo.m_width + x];
            rowBuffer[x] = adjustPixel(gray);
          }
        } else if (oneBit) {
          // 1-bit output with Atkinson dithering for better quality
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = mcuRowBuffer[bufferY * imageInfo.m_width + x];
            const uint8_t bit =
                atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(gray, x) : quantize1bit(gray, x, y);
            // Pack 1-bit value: MSB first, 8 pixels per byte.
            const int byteIndex = x >> 3;
            const int bitOffset = 7 - (x & 7);
            rowBuffer[byteIndex] |= (bit << bitOffset);
          }
          if (atkinson1BitDitherer) atkinson1BitDitherer->nextRow();
        } else {
          // 2-bit output
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = adjustPixel(mcuRowBuffer[bufferY * imageInfo.m_width + x]);
            uint8_t twoBit;
            if (quickMode) {
              // Quick mode: simple threshold (faster, no dithering)
              twoBit = quantizeSimple(gray);
            } else if (atkinsonDitherer) {
              twoBit = atkinsonDitherer->processPixel(gray, x);
            } else if (fsDitherer) {
              twoBit = fsDitherer->processPixel(gray, x);
            } else {
              twoBit = quantize(gray, x, y);
            }
            // 2-bit packing: x*2/8 → x>>2; (x*2) % 8 → (x & 3) << 1.
            const int byteIndex = x >> 2;
            const int bitOffset = 6 - ((x & 3) << 1);
            rowBuffer[byteIndex] |= (twoBit << bitOffset);
          }
          if (atkinsonDitherer)
            atkinsonDitherer->nextRow();
          else if (fsDitherer)
            fsDitherer->nextRow();
        }
        {
          snapix::spi::SharedBusLock busLock;
          if (!busLock || bmpOut.write(rowBuffer, bytesPerRow) != static_cast<size_t>(bytesPerRow)) {
            LOG_ERR(TAG, "Failed to write BMP row");
            if (rowAccum) delete[] rowAccum;
            if (rowCount) delete[] rowCount;
            if (atkinsonDitherer) delete atkinsonDitherer;
            if (fsDitherer) delete fsDitherer;
            if (atkinson1BitDitherer) delete atkinson1BitDitherer;
            free(mcuRowBuffer);
            free(rowBuffer);
            return false;
          }
        }
      } else {
        // Fixed-point area averaging for exact fit scaling
        // For each output pixel X, accumulate source pixels that map to it
        // srcX range for outX: [outX * scaleX_fp >> 16, (outX+1) * scaleX_fp >> 16)
        const uint8_t* srcRow = mcuRowBuffer + bufferY * imageInfo.m_width;

        for (int outX = 0; outX < outWidth; outX++) {
          // Calculate source X range for this output pixel
          const int srcXStart = (static_cast<uint32_t>(outX) * scaleX_fp) >> 16;
          const int srcXEnd = (static_cast<uint32_t>(outX + 1) * scaleX_fp) >> 16;

          // Accumulate all source pixels in this range
          int sum = 0;
          int count = 0;
          for (int srcX = srcXStart; srcX < srcXEnd && srcX < imageInfo.m_width; srcX++) {
            sum += srcRow[srcX];
            count++;
          }

          // Handle edge case: if no pixels in range, use nearest
          if (count == 0 && srcXStart < imageInfo.m_width) {
            sum = srcRow[srcXStart];
            count = 1;
          }

          rowAccum[outX] += sum;
          rowCount[outX] += count;
        }

        // Check if we've crossed into the next output row
        // Current source Y in fixed point: y << 16
        const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;

        // Output row when source Y crosses the boundary
        if (srcY_fp >= nextOutY_srcStart && currentOutY < outHeight) {
          memset(rowBuffer, 0, bytesPerRow);

          if (USE_8BIT_OUTPUT && !oneBit) {
            for (int x = 0; x < outWidth; x++) {
              const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
              rowBuffer[x] = adjustPixel(gray);
            }
          } else if (oneBit) {
            // 1-bit output with Atkinson dithering for better quality
            for (int x = 0; x < outWidth; x++) {
              const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
              const uint8_t bit = atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(gray, x)
                                                       : quantize1bit(gray, x, currentOutY);
              // Pack 1-bit value: MSB first, 8 pixels per byte.
              const int byteIndex = x >> 3;
              const int bitOffset = 7 - (x & 7);
              rowBuffer[byteIndex] |= (bit << bitOffset);
            }
            if (atkinson1BitDitherer) atkinson1BitDitherer->nextRow();
          } else {
            // 2-bit output
            for (int x = 0; x < outWidth; x++) {
              const uint8_t gray = adjustPixel((rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0);
              uint8_t twoBit;
              if (quickMode) {
                // Quick mode: simple threshold (faster, no dithering)
                twoBit = quantizeSimple(gray);
              } else if (atkinsonDitherer) {
                twoBit = atkinsonDitherer->processPixel(gray, x);
              } else if (fsDitherer) {
                twoBit = fsDitherer->processPixel(gray, x);
              } else {
                twoBit = quantize(gray, x, currentOutY);
              }
              // 2-bit packing: see jpegFileToBmpStreamInternal non-scaled path.
              const int byteIndex = x >> 2;
              const int bitOffset = 6 - ((x & 3) << 1);
              rowBuffer[byteIndex] |= (twoBit << bitOffset);
            }
            if (atkinsonDitherer)
              atkinsonDitherer->nextRow();
            else if (fsDitherer)
              fsDitherer->nextRow();
          }

          {
            snapix::spi::SharedBusLock busLock;
            if (!busLock || bmpOut.write(rowBuffer, bytesPerRow) != static_cast<size_t>(bytesPerRow)) {
              LOG_ERR(TAG, "Failed to write scaled BMP row");
              if (rowAccum) delete[] rowAccum;
              if (rowCount) delete[] rowCount;
              if (atkinsonDitherer) delete atkinsonDitherer;
              if (fsDitherer) delete fsDitherer;
              if (atkinson1BitDitherer) delete atkinson1BitDitherer;
              free(mcuRowBuffer);
              free(rowBuffer);
              return false;
            }
          }
          currentOutY++;

          // Reset accumulators for next output row
          memset(rowAccum, 0, outWidth * sizeof(uint32_t));
          memset(rowCount, 0, outWidth * sizeof(uint16_t));

          // Update boundary for next output row
          nextOutY_srcStart = static_cast<uint32_t>(currentOutY + 1) * scaleY_fp;
        }
      }
    }
  }

  // Clean up
  if (rowAccum) {
    delete[] rowAccum;
  }
  if (rowCount) {
    delete[] rowCount;
  }
  if (atkinsonDitherer) {
    delete atkinsonDitherer;
  }
  if (fsDitherer) {
    delete fsDitherer;
  }
  if (atkinson1BitDitherer) {
    delete atkinson1BitDitherer;
  }
  free(mcuRowBuffer);
  free(rowBuffer);

  LOG_INF(TAG, "Successfully converted JPEG to BMP");
  return true;
}

// Core function: Convert JPEG file to 2-bit BMP (uses default target size)
bool JpegToBmpConverter::jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, TARGET_MAX_WIDTH, TARGET_MAX_HEIGHT, false);
}

// Convert with custom target size (for thumbnails, 2-bit)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight, const std::function<bool()>& shouldAbort) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, false, false, shouldAbort);
}

// Convert to 1-bit BMP (black and white only, no grays) using default target size
bool JpegToBmpConverter::jpegFileTo1BitBmpStream(FsFile& jpegFile, Print& bmpOut) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, TARGET_MAX_WIDTH, TARGET_MAX_HEIGHT, true);
}

// Convert to 1-bit BMP with custom target size (for thumbnails)
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, true);
}

// Quick preview mode: simple threshold instead of dithering (faster but lower quality)
bool JpegToBmpConverter::jpegFileToBmpStreamQuick(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                  int targetMaxHeight, const std::function<bool()>& shouldAbort) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, false, true, shouldAbort);
}

// Header-only peek: just enough work to extract image dimensions.  picojpeg's
// pjpeg_decode_init reads up to the SOF marker and returns image_info — it
// does NOT decode any pixels, so this is an order of magnitude cheaper than
// jpegFileToBmpStream*.
bool JpegToBmpConverter::peekDimensions(FsFile& jpegFile, int& outWidth, int& outHeight) {
  outWidth = 0;
  outHeight = 0;

  if (!jpegFile) return false;

  // Reset to start of file so peek is idempotent (callers may have read header bytes).
  {
    snapix::spi::SharedBusLock lk;
    if (!jpegFile.seek(0)) return false;
  }

  // Reject progressive / arithmetic-coded JPEGs early — picojpeg can't handle them.
  if (isUnsupportedJpeg(jpegFile)) {
    LOG_DBG(TAG, "peekDimensions: unsupported JPEG (progressive / arithmetic)");
    return false;
  }

  std::unique_ptr<uint8_t[]> jpegBuf(new (std::nothrow) uint8_t[JpegReadContext::kBufferSize]);
  if (!jpegBuf) return false;
  JpegReadContext context = {.file = jpegFile, .buffer = jpegBuf.get(), .bufferPos = 0, .bufferFilled = 0};
  pjpeg_image_info_t imageInfo;
  const unsigned char status = pjpeg_decode_init(&imageInfo, jpegReadCallback, &context, 0);
  if (status != 0) {
    LOG_DBG(TAG, "peekDimensions: decode_init failed status=%d", status);
    return false;
  }
  if (imageInfo.m_width == 0 || imageInfo.m_height == 0) {
    return false;
  }
  outWidth = imageInfo.m_width;
  outHeight = imageInfo.m_height;
  return true;
}

// Preview decode using picojpeg's reduce=1 mode.  picojpeg still walks the
// Huffman bitstream (mandatory — it's stateful across MCUs), but skips the
// per-pixel AC dequant + IDCT + chroma upsampling for each 8×8 block.
// transformBlockReduce() (picojpeg.c:1728) writes only the [0] pixel of each
// MCU buffer, so we sample one pixel per MCU and emit a tiny BMP whose linear
// dimensions are 1/8 of the original.  drawBitmap() upscales it on render.
//
// On a typical 650×880 JPEG (~6 s full decode) this returns in ~0.7-1 s —
// the FB2 BG worker writes it as <id>.preview.bmp before queuing the full
// decode in a follow-up sweep, so the user sees a blurry-but-located image
// well before the final pixels arrive.
bool JpegToBmpConverter::jpegFileToBmpStreamPreview(FsFile& jpegFile, Print& bmpOut) {
  if (!jpegFile) return false;

  {
    snapix::spi::SharedBusLock lk;
    if (!jpegFile.seek(0)) return false;
  }

  if (isUnsupportedJpeg(jpegFile)) {
    LOG_DBG(TAG, "preview: unsupported JPEG");
    return false;
  }

  std::unique_ptr<uint8_t[]> jpegBuf(new (std::nothrow) uint8_t[JpegReadContext::kBufferSize]);
  if (!jpegBuf) {
    LOG_ERR(TAG, "preview: OOM allocating %u-byte read buffer",
            static_cast<unsigned>(JpegReadContext::kBufferSize));
    return false;
  }
  JpegReadContext context = {.file = jpegFile, .buffer = jpegBuf.get(), .bufferPos = 0, .bufferFilled = 0};
  pjpeg_image_info_t imageInfo;
  if (pjpeg_decode_init(&imageInfo, jpegReadCallback, &context, /*reduce=*/1) != 0) {
    LOG_DBG(TAG, "preview: decode_init failed");
    return false;
  }
  if (imageInfo.m_width == 0 || imageInfo.m_height == 0 || imageInfo.m_MCUSPerRow == 0 ||
      imageInfo.m_MCUSPerCol == 0 || imageInfo.m_MCUWidth == 0 || imageInfo.m_MCUHeight == 0) {
    return false;
  }

  // For typical YH2V2 JPEGs the MCU is 16×16 = 4 Y blocks, and reduce mode
  // writes one valid pixel per block at byte offsets 0, 64, 128, 192 of the
  // R/G/B MCU buffers (see picojpeg.c:1847).  Extracting all of them gives
  // 2× linear resolution vs sampling only [0] — turns a 31×26 ant farm into
  // a 62×52 preview that's actually recognisable.  YH2V1 / YH1V2 / YH1V1 /
  // GRAYSCALE work the same way with 1 or 2 blocks per axis.
  const int blocksPerMCUx = imageInfo.m_MCUWidth >> 3;   // 1 or 2
  const int blocksPerMCUy = imageInfo.m_MCUHeight >> 3;  // 1 or 2
  const int outW = imageInfo.m_MCUSPerRow * blocksPerMCUx;
  const int outH = imageInfo.m_MCUSPerCol * blocksPerMCUy;
  if (outW <= 0 || outH <= 0 || outW > 4096 || outH > 4096) {
    return false;
  }
  const int bytesPerRow = ((outW * 2 + 31) / 32) * 4;

  // We need the full preview row buffer alive across all MCUs in a row so
  // their multiple block contributions can be packed before flushing.
  // bytesPerRow × blocksPerMCUy buffers because YH*V2 MCUs span 2 output
  // rows and we don't want to decode each MCU twice.
  const int rowsPerMcuBand = blocksPerMCUy;
  uint8_t* outRows = static_cast<uint8_t*>(malloc(static_cast<size_t>(bytesPerRow) * rowsPerMcuBand));
  if (!outRows) {
    LOG_ERR(TAG, "preview: malloc(%d) failed", bytesPerRow * rowsPerMcuBand);
    return false;
  }

  writeBmpHeader2bit(bmpOut, outW, outH);

  // Standard 4×4 Bayer ordered-dither matrix scaled to 0..255.  Picks a
  // different threshold per output pixel so DC values that cluster near
  // bright still produce visible black stipples instead of vanishing.
  static const uint8_t kBayer4[4][4] = {{16,  144, 48,  176},
                                        {208, 80,  240, 112},
                                        {64,  192, 32,  160},
                                        {255, 128, 224, 96}};

  bool ok = true;
  for (int mcuY = 0; mcuY < imageInfo.m_MCUSPerCol && ok; mcuY++) {
    memset(outRows, 0, static_cast<size_t>(bytesPerRow) * rowsPerMcuBand);
    bool earlyEnd = false;
    for (int mcuX = 0; mcuX < imageInfo.m_MCUSPerRow; mcuX++) {
      const unsigned char status = pjpeg_decode_mcu();
      if (status != 0) {
        if (status == PJPG_NO_MORE_BLOCKS) {
          earlyEnd = true;
          break;
        }
        LOG_ERR(TAG, "preview: decode_mcu err=%d at mcu=%d,%d", status, mcuX, mcuY);
        ok = false;
        break;
      }
      // Emit blocksPerMCUx × blocksPerMCUy pixels for this MCU.
      for (int by = 0; by < blocksPerMCUy; by++) {
        for (int bx = 0; bx < blocksPerMCUx; bx++) {
          const int blockOffset = by * 128 + bx * 64;
          uint8_t gray;
          if (imageInfo.m_comps == 1) {
            gray = imageInfo.m_pMCUBufR[blockOffset];
          } else {
            gray = rgbToGray(imageInfo.m_pMCUBufR[blockOffset], imageInfo.m_pMCUBufG[blockOffset],
                             imageInfo.m_pMCUBufB[blockOffset]);
          }
          const int outX = mcuX * blocksPerMCUx + bx;
          const uint8_t threshold = kBayer4[(mcuY * blocksPerMCUy + by) & 3][outX & 3];
          const uint8_t twoBit = (gray < threshold) ? 0 : 3;
          uint8_t* dst = outRows + by * bytesPerRow;
          dst[outX >> 2] |= (twoBit << (6 - ((outX & 3) << 1)));
        }
      }
    }
    {
      snapix::spi::SharedBusLock lk;
      if (!lk || bmpOut.write(outRows, static_cast<size_t>(bytesPerRow) * rowsPerMcuBand) !=
                     static_cast<size_t>(bytesPerRow) * rowsPerMcuBand) {
        ok = false;
      }
    }
    if (earlyEnd) break;
    if ((mcuY & 0x7) == 0) taskYIELD();
  }

  free(outRows);
  if (ok) {
    LOG_INF(TAG, "preview: %dx%d decoded", outW, outH);
  }
  return ok;
}
