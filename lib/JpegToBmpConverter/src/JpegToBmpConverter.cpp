#include "JpegToBmpConverter.h"

#include <Arduino.h>
#include <Logging.h>

#define TAG "JPEG"
#include <SdFat.h>
#include <SharedSpiLock.h>
#include <tjpgd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "BitmapHelpers.h"

// ---------------------------------------------------------------------------
// TJpgDec input pump context.
//
// Same 4 KB pump-buffer pattern as the picojpeg-era code.  TJpgDec's input
// callback can request arbitrary chunk sizes (up to JD_SZBUF = 4096), but it
// will still re-enter many times across a single decode — every refill that
// reaches SDFat must hold the SharedBusLock briefly.  We hide those refills
// behind a 4 KB user-space buffer so the lock is acquired ~1× per FAT cluster
// instead of ~1× per Huffman fetch.  4 KB matches one SDFat block, so each
// read fills exactly one cache line.
//
// On a 150 KB JPEG that's ~37 SD reads instead of hundreds — saves 20–50 ms
// of SharedBusLock contention when the BG cache worker is also writing.  The
// buffer is heap-allocated (RAII via std::unique_ptr) to avoid bursting the
// 16 KB BG worker stack with a 4 KB local.
// ---------------------------------------------------------------------------
struct JpegReadContext {
  FsFile& file;
  static constexpr size_t kBufferSize = 4096;
  uint8_t* buffer;
  size_t bufferPos;
  size_t bufferFilled;
};

// TJpgDec input function — pulls bytes from the SD-backed FsFile through our
// 4 KB pump buffer.  When buff==nullptr we just advance the file (skip).
static size_t tjpgInputCb(JDEC* jd, uint8_t* buff, size_t nbyte) {
  auto* context = static_cast<JpegReadContext*>(jd->device);
  if (!context || !context->file) return 0;

  if (buff == nullptr) {
    // Skip mode — advance the file pointer by `nbyte` without buffering.
    // Best to flush our buffer first (its contents are stale after a seek)
    // and seek directly so we don't waste time copying skipped bytes through
    // the buffer.
    size_t skipped = 0;
    // Drain whatever is already buffered.
    const size_t buffered = context->bufferFilled - context->bufferPos;
    const size_t fromBuf = buffered < nbyte ? buffered : nbyte;
    context->bufferPos += fromBuf;
    skipped += fromBuf;
    if (skipped < nbyte) {
      const size_t remain = nbyte - skipped;
      snapix::spi::SharedBusLock busLock;
      if (!busLock) return skipped;
      const uint64_t pos = context->file.position();
      if (!context->file.seek(pos + remain)) return skipped;
      skipped += remain;
    }
    return skipped;
  }

  // Read mode — copy `nbyte` bytes into `buff`, refilling our 4 KB buffer as
  // needed.  Loops because `nbyte` (up to JD_SZBUF=4096) can require more
  // than one buffer worth if alignment is unfavorable.
  size_t copied = 0;
  while (copied < nbyte) {
    if (context->bufferPos >= context->bufferFilled) {
      snapix::spi::SharedBusLock busLock;
      if (!busLock) break;
      const int readResult = context->file.read(context->buffer, JpegReadContext::kBufferSize);
      context->bufferPos = 0;
      if (readResult <= 0) {
        context->bufferFilled = 0;
        break;  // EOF or error — return short read
      }
      context->bufferFilled = static_cast<size_t>(readResult);
    }
    const size_t available = context->bufferFilled - context->bufferPos;
    const size_t want = nbyte - copied;
    const size_t take = available < want ? available : want;
    memcpy(buff + copied, context->buffer + context->bufferPos, take);
    context->bufferPos += take;
    copied += take;
  }
  return copied;
}

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

// Stub kept solely so the static declaration in JpegToBmpConverter.h still
// resolves at link time.  TJpgDec uses its own input callback (tjpgInputCb)
// with a different signature; this old picojpeg-flavoured callback is no
// longer wired to anything.
unsigned char JpegToBmpConverter::jpegReadCallback(unsigned char* /*pBuf*/, const unsigned char /*buf_size*/,
                                                   unsigned char* pBytes_actually_read,
                                                   void* /*pCallback_data*/) {
  if (pBytes_actually_read) *pBytes_actually_read = 0;
  return 0;
}

// ---------------------------------------------------------------------------
// MCU-row-buffered streaming context for the TJpgDec output callback.
//
// TJpgDec calls our outfunc once per decoded MCU rectangle, sweeping the
// output image in raster order: left-to-right within an MCU row, then top-
// to-bottom.  An MCU row spans `mcuPixelHeight` decoded rows, so we
// accumulate one MCU-row of pixels, then flush all of them through the
// post-scale + dither + 2-bit-pack path before moving on.
// ---------------------------------------------------------------------------
struct JpegDecodeContext {
  // Source (after TJpgDec's hardware scale).
  int decodedWidth;        // jd->width >> scale
  int decodedHeight;       // jd->height >> scale
  int mcuPixelWidth;       // (jd->msx * 8) >> scale
  int mcuPixelHeight;      // (jd->msy * 8) >> scale

  // MCU row buffer (one row of MCUs worth of pixels).
  uint8_t* mcuRowBuffer;   // decodedWidth × mcuPixelHeight bytes
  int mcuRowBufferRows;    // == mcuPixelHeight (capacity)
  int currentMcuRowTop;    // y of the current MCU row in decoded space (multiple of mcuPixelHeight)
  bool mcuRowDirty;        // any pixels written to the current MCU row?

  // Output image dimensions (after software post-scale).
  int outWidth;
  int outHeight;
  uint8_t* rowBuffer;      // packed BMP output row
  int bytesPerRow;

  // Software post-scale state (decoded -> output).  scale*_fp is in 16.16
  // fixed point and represents "decoded source pixels per output pixel".
  bool needsScaling;
  uint32_t scaleX_fp;
  uint32_t scaleY_fp;
  uint32_t* rowAccum;       // per-output-X sum of source pixels
  uint16_t* rowCount;       // per-output-X count of source pixels accumulated
  int currentOutY;
  uint32_t nextOutY_srcStart;  // boundary (16.16 fixed point) where we flush an output row

  // Dither/quantization state.
  bool oneBit;
  bool quickMode;
  AtkinsonDitherer* atkinsonDitherer;
  FloydSteinbergDitherer* fsDitherer;
  Atkinson1BitDitherer* atkinson1BitDitherer;

  // Output sink.
  Print* bmpOut;
  bool writeError;

  // Cooperative-abort knobs.
  const std::function<bool()>* shouldAbort;
  bool aborted;
  int mcuRowsProcessed;     // for periodic taskYIELD
};

// Flush a finished decoded source row through the post-scale + dither + pack
// pipeline and write one (or more) output BMP rows.  `srcY` is the y
// coordinate (in decoded space) of the row being flushed.
static bool emitDecodedRow(JpegDecodeContext& ctx, int srcY) {
  const uint8_t* srcRow = ctx.mcuRowBuffer + (srcY - ctx.currentMcuRowTop) * ctx.decodedWidth;

  if (!ctx.needsScaling) {
    memset(ctx.rowBuffer, 0, ctx.bytesPerRow);

    if (USE_8BIT_OUTPUT && !ctx.oneBit) {
      for (int x = 0; x < ctx.outWidth; x++) {
        ctx.rowBuffer[x] = adjustPixel(srcRow[x]);
      }
    } else if (ctx.oneBit) {
      for (int x = 0; x < ctx.outWidth; x++) {
        const uint8_t gray = srcRow[x];
        const uint8_t bit = ctx.atkinson1BitDitherer ? ctx.atkinson1BitDitherer->processPixel(gray, x)
                                                     : quantize1bit(gray, x, srcY);
        const int byteIndex = x >> 3;
        const int bitOffset = 7 - (x & 7);
        ctx.rowBuffer[byteIndex] |= (bit << bitOffset);
      }
      if (ctx.atkinson1BitDitherer) ctx.atkinson1BitDitherer->nextRow();
    } else {
      for (int x = 0; x < ctx.outWidth; x++) {
        const uint8_t gray = adjustPixel(srcRow[x]);
        uint8_t twoBit;
        if (ctx.quickMode) {
          twoBit = quantizeSimple(gray);
        } else if (ctx.atkinsonDitherer) {
          twoBit = ctx.atkinsonDitherer->processPixel(gray, x);
        } else if (ctx.fsDitherer) {
          twoBit = ctx.fsDitherer->processPixel(gray, x);
        } else {
          twoBit = quantize(gray, x, srcY);
        }
        // 2-bit packing: x*2/8 → x>>2; (x*2) % 8 → (x & 3) << 1.
        const int byteIndex = x >> 2;
        const int bitOffset = 6 - ((x & 3) << 1);
        ctx.rowBuffer[byteIndex] |= (twoBit << bitOffset);
      }
      if (ctx.atkinsonDitherer)
        ctx.atkinsonDitherer->nextRow();
      else if (ctx.fsDitherer)
        ctx.fsDitherer->nextRow();
    }

    snapix::spi::SharedBusLock busLock;
    if (!busLock || ctx.bmpOut->write(ctx.rowBuffer, ctx.bytesPerRow) != static_cast<size_t>(ctx.bytesPerRow)) {
      LOG_ERR(TAG, "Failed to write BMP row");
      ctx.writeError = true;
      return false;
    }
    return true;
  }

  // Scaling path: accumulate decoded pixels into per-output-X sums, then
  // flush whenever we cross a vertical output-row boundary.
  for (int outX = 0; outX < ctx.outWidth; outX++) {
    const int srcXStart = (static_cast<uint32_t>(outX) * ctx.scaleX_fp) >> 16;
    const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx.scaleX_fp) >> 16;
    int sum = 0;
    int count = 0;
    for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx.decodedWidth; srcX++) {
      sum += srcRow[srcX];
      count++;
    }
    if (count == 0 && srcXStart < ctx.decodedWidth) {
      sum = srcRow[srcXStart];
      count = 1;
    }
    ctx.rowAccum[outX] += sum;
    ctx.rowCount[outX] += count;
  }

  const uint32_t srcY_fp = static_cast<uint32_t>(srcY + 1) << 16;
  if (srcY_fp >= ctx.nextOutY_srcStart && ctx.currentOutY < ctx.outHeight) {
    memset(ctx.rowBuffer, 0, ctx.bytesPerRow);

    if (USE_8BIT_OUTPUT && !ctx.oneBit) {
      for (int x = 0; x < ctx.outWidth; x++) {
        const uint8_t gray = (ctx.rowCount[x] > 0) ? (ctx.rowAccum[x] / ctx.rowCount[x]) : 0;
        ctx.rowBuffer[x] = adjustPixel(gray);
      }
    } else if (ctx.oneBit) {
      for (int x = 0; x < ctx.outWidth; x++) {
        const uint8_t gray = (ctx.rowCount[x] > 0) ? (ctx.rowAccum[x] / ctx.rowCount[x]) : 0;
        const uint8_t bit = ctx.atkinson1BitDitherer ? ctx.atkinson1BitDitherer->processPixel(gray, x)
                                                     : quantize1bit(gray, x, ctx.currentOutY);
        const int byteIndex = x >> 3;
        const int bitOffset = 7 - (x & 7);
        ctx.rowBuffer[byteIndex] |= (bit << bitOffset);
      }
      if (ctx.atkinson1BitDitherer) ctx.atkinson1BitDitherer->nextRow();
    } else {
      for (int x = 0; x < ctx.outWidth; x++) {
        const uint8_t gray = adjustPixel((ctx.rowCount[x] > 0) ? (ctx.rowAccum[x] / ctx.rowCount[x]) : 0);
        uint8_t twoBit;
        if (ctx.quickMode) {
          twoBit = quantizeSimple(gray);
        } else if (ctx.atkinsonDitherer) {
          twoBit = ctx.atkinsonDitherer->processPixel(gray, x);
        } else if (ctx.fsDitherer) {
          twoBit = ctx.fsDitherer->processPixel(gray, x);
        } else {
          twoBit = quantize(gray, x, ctx.currentOutY);
        }
        // 2-bit packing: see jpegFileToBmpStreamInternal non-scaled path.
        const int byteIndex = x >> 2;
        const int bitOffset = 6 - ((x & 3) << 1);
        ctx.rowBuffer[byteIndex] |= (twoBit << bitOffset);
      }
      if (ctx.atkinsonDitherer)
        ctx.atkinsonDitherer->nextRow();
      else if (ctx.fsDitherer)
        ctx.fsDitherer->nextRow();
    }

    {
      snapix::spi::SharedBusLock busLock;
      if (!busLock || ctx.bmpOut->write(ctx.rowBuffer, ctx.bytesPerRow) != static_cast<size_t>(ctx.bytesPerRow)) {
        LOG_ERR(TAG, "Failed to write scaled BMP row");
        ctx.writeError = true;
        return false;
      }
    }
    ctx.currentOutY++;
    memset(ctx.rowAccum, 0, ctx.outWidth * sizeof(uint32_t));
    memset(ctx.rowCount, 0, ctx.outWidth * sizeof(uint16_t));
    ctx.nextOutY_srcStart = static_cast<uint32_t>(ctx.currentOutY + 1) * ctx.scaleY_fp;
  }
  return true;
}

// Flush all currently-buffered decoded rows in the active MCU row.
static bool flushMcuRow(JpegDecodeContext& ctx) {
  if (!ctx.mcuRowDirty) return true;
  const int top = ctx.currentMcuRowTop;
  for (int y = top; y < top + ctx.mcuPixelHeight && y < ctx.decodedHeight; y++) {
    if (!emitDecodedRow(ctx, y)) return false;
  }
  ctx.mcuRowDirty = false;
  return true;
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

  // Setup pump buffer for TJpgDec input callback.  4 KB on heap (RAII via
  // unique_ptr) — see JpegReadContext docstring above for why.
  std::unique_ptr<uint8_t[]> jpegBuf(new (std::nothrow) uint8_t[JpegReadContext::kBufferSize]);
  if (!jpegBuf) {
    LOG_ERR(TAG, "JPEG decoder OOM allocating %u-byte read buffer",
            static_cast<unsigned>(JpegReadContext::kBufferSize));
    return false;
  }
  JpegReadContext readCtx = {.file = jpegFile, .buffer = jpegBuf.get(), .bufferPos = 0, .bufferFilled = 0};

  // Allocate TJpgDec workspace (~10 KB with JD_FASTDECODE=2).
  std::unique_ptr<uint8_t[]> pool(new (std::nothrow) uint8_t[TJPGD_WORKSPACE_SIZE]);
  if (!pool) {
    LOG_ERR(TAG, "JPEG decoder OOM allocating %d-byte TJpgDec workspace", static_cast<int>(TJPGD_WORKSPACE_SIZE));
    return false;
  }

  // jd_prepare reads up to (and including) the SOF marker, populating
  // jd->width/height/msx/msy.  No pixel data is decoded yet.
  JDEC jd;
  JRESULT prep = jd_prepare(&jd, tjpgInputCb, pool.get(), TJPGD_WORKSPACE_SIZE, &readCtx);
  if (prep != JDR_OK) {
    LOG_ERR(TAG, "JPEG jd_prepare failed: %d", static_cast<int>(prep));
    return false;
  }

  const int srcW = jd.width;
  const int srcH = jd.height;
  const int mcuPxW = static_cast<int>(jd.msx) * 8;
  const int mcuPxH = static_cast<int>(jd.msy) * 8;
  LOG_INF(TAG, "JPEG dimensions: %dx%d, components: %d, MCU: %dx%d", srcW, srcH, static_cast<int>(jd.ncomp), mcuPxW,
          mcuPxH);

  // Safety limits to prevent memory issues on ESP32
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  constexpr int MAX_MCU_ROW_BYTES = 65536;

  if (srcW > MAX_IMAGE_WIDTH || srcH > MAX_IMAGE_HEIGHT) {
    LOG_ERR(TAG, "Image too large (%dx%d), max supported: %dx%d", srcW, srcH, MAX_IMAGE_WIDTH, MAX_IMAGE_HEIGHT);
    return false;
  }
  // Defensive: a malformed JPEG can still report 0×0 / 0-dimension MCUs after
  // a "successful" jd_prepare.  Reject before any divide-by-zero or zero-byte
  // allocation downstream.
  if (srcW == 0 || srcH == 0 || mcuPxW == 0 || mcuPxH == 0) {
    LOG_ERR(TAG, "Malformed JPEG: zero dimension(s) %dx%d MCU=%dx%d", srcW, srcH, mcuPxW, mcuPxH);
    return false;
  }

  // Choose TJpgDec hardware scale.  Native scale steps: 0=1/1, 1=1/2, 2=1/4,
  // 3=1/8.  Picking the largest scale whose decoded dimension is still
  // >= the target avoids wasted decode effort while preserving headroom for
  // accurate post-scale.  When no target is given we keep scale=0.
  uint8_t hwScale = 0;
  if (targetWidth > 0 && targetHeight > 0 && (srcW > targetWidth || srcH > targetHeight)) {
    // Find the largest k such that (srcW>>k) >= targetWidth and (srcH>>k) >= targetHeight.
    for (uint8_t k = 1; k <= 3; k++) {
      const int dw = srcW >> k;
      const int dh = srcH >> k;
      if (dw >= targetWidth && dh >= targetHeight && dw > 0 && dh > 0) {
        hwScale = k;
      } else {
        break;
      }
    }
  }

  const int decodedW = srcW >> hwScale;
  const int decodedH = srcH >> hwScale;
  const int decodedMcuW = mcuPxW >> hwScale;
  const int decodedMcuH = mcuPxH >> hwScale;
  if (decodedW <= 0 || decodedH <= 0 || decodedMcuW <= 0 || decodedMcuH <= 0) {
    LOG_ERR(TAG, "Decoded dimensions degenerate after scale=%d (%dx%d, MCU %dx%d)", static_cast<int>(hwScale),
            decodedW, decodedH, decodedMcuW, decodedMcuH);
    return false;
  }

  // Software post-scale: pure downscale to fit within the requested box while
  // preserving aspect ratio.  Same fixed-point area-averaging math as the
  // picojpeg-era code.
  int outWidth = decodedW;
  int outHeight = decodedH;
  uint32_t scaleX_fp = 65536;
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (targetWidth > 0 && targetHeight > 0 && (decodedW > targetWidth || decodedH > targetHeight)) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / decodedW;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / decodedH;
    const float scale = std::min(scaleToFitWidth, scaleToFitHeight);

    outWidth = static_cast<int>(decodedW * scale);
    outHeight = static_cast<int>(decodedH * scale);
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    scaleX_fp = (static_cast<uint32_t>(decodedW) << 16) / outWidth;
    scaleY_fp = (static_cast<uint32_t>(decodedH) << 16) / outHeight;
    needsScaling = true;

    LOG_INF(TAG, "Pre-scaling %dx%d -> %dx%d (fit to %dx%d, hwScale=%d)", srcW, srcH, outWidth, outHeight,
            targetWidth, targetHeight, static_cast<int>(hwScale));
  } else if (hwScale > 0) {
    LOG_INF(TAG, "Hardware scale 1/%d: %dx%d -> %dx%d", 1 << hwScale, srcW, srcH, decodedW, decodedH);
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

  // Allocate output row buffer
  auto* rowBuffer = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuffer) {
    LOG_ERR(TAG, "Failed to allocate row buffer");
    return false;
  }

  // Allocate the MCU-row buffer: one row of MCUs in decoded (post-hardware-
  // scale) coordinates.  This is the working area between TJpgDec's per-rect
  // calls and our per-row dither/pack pipeline.
  const int mcuRowPixels = decodedW * decodedMcuH;
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
  memset(mcuRowBuffer, 0, mcuRowPixels);

  // Create ditherer if enabled (skip in quickMode for faster preview).  Use
  // OUTPUT (post-scale) dimensions for dithering — error diffusion lattice
  // must match the row width that's actually being packed.
  AtkinsonDitherer* atkinsonDitherer = nullptr;
  FloydSteinbergDitherer* fsDitherer = nullptr;
  Atkinson1BitDitherer* atkinson1BitDitherer = nullptr;

  if (!quickMode) {
    if (oneBit) {
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

  // For scaling we accumulate decoded source rows into the current output
  // row.  rowAccum/rowCount track the per-output-X running sum.
  uint32_t* rowAccum = nullptr;
  uint16_t* rowCount = nullptr;
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
  }

  // Build the decode context and rebind jd->device to it.  TJpgDec only knows
  // about a single `device` slot; we used it for JpegReadContext during
  // jd_prepare and now point it at our JpegDecodeContext for jd_decomp.  The
  // tjpgInputCb dereferences jd->device — but we no longer call infunc from
  // outfunc, only from inside jd_decomp's bitstream path.  So during decomp
  // both callbacks need the right device.  Solution: keep both contexts in
  // a wrapper that routes by callback.
  //
  // Simpler: TJpgDec stores `device` once in jd->device.  Our infunc dereferences
  // jd->device as JpegReadContext, and outfunc dereferences it as
  // JpegDecodeContext.  So we MUST rebind between calls.  Conveniently
  // jd_prepare doesn't call outfunc and jd_decomp's infunc only fires inside
  // mcu_load (between outfunc calls).  Instead of dual rebinds we keep both
  // contexts alive and the input callback gets a separate path: we stuff the
  // read context pointer into the decode context too, and the input callback
  // walks via that.
  //
  // Implementation: jd->device holds JpegDecodeContext*; the decode context
  // carries a pointer to JpegReadContext.  Adapt input callback accordingly.
  JpegDecodeContext decodeCtx = {};
  decodeCtx.decodedWidth = decodedW;
  decodeCtx.decodedHeight = decodedH;
  decodeCtx.mcuPixelWidth = decodedMcuW;
  decodeCtx.mcuPixelHeight = decodedMcuH;
  decodeCtx.mcuRowBuffer = mcuRowBuffer;
  decodeCtx.mcuRowBufferRows = decodedMcuH;
  decodeCtx.currentMcuRowTop = 0;
  decodeCtx.mcuRowDirty = false;
  decodeCtx.outWidth = outWidth;
  decodeCtx.outHeight = outHeight;
  decodeCtx.rowBuffer = rowBuffer;
  decodeCtx.bytesPerRow = bytesPerRow;
  decodeCtx.needsScaling = needsScaling;
  decodeCtx.scaleX_fp = scaleX_fp;
  decodeCtx.scaleY_fp = scaleY_fp;
  decodeCtx.rowAccum = rowAccum;
  decodeCtx.rowCount = rowCount;
  decodeCtx.currentOutY = 0;
  decodeCtx.nextOutY_srcStart = scaleY_fp;  // first boundary at decoded-Y == scaleY_fp >> 16
  decodeCtx.oneBit = oneBit;
  decodeCtx.quickMode = quickMode;
  decodeCtx.atkinsonDitherer = atkinsonDitherer;
  decodeCtx.fsDitherer = fsDitherer;
  decodeCtx.atkinson1BitDitherer = atkinson1BitDitherer;
  decodeCtx.bmpOut = &bmpOut;
  decodeCtx.writeError = false;
  decodeCtx.shouldAbort = (shouldAbort ? &shouldAbort : nullptr);
  decodeCtx.aborted = false;
  decodeCtx.mcuRowsProcessed = 0;

  // To dispatch infunc/outfunc from the same jd->device slot, we use a tiny
  // adapter struct that holds both pointers.
  struct DualCtx {
    JpegReadContext* read;
    JpegDecodeContext* decode;
  } dual = {&readCtx, &decodeCtx};

  // Rewire input callback to use the dual adapter.  We replace tjpgInputCb's
  // jd->device dereference with one that pulls .read from the adapter.  This
  // is done via a thin wrapper lambda — but since TJpgDec's callback is a C
  // function pointer with no captures, we use a static-trampoline via the
  // adapter pattern: jd->device = &dual; both callbacks do
  // static_cast<DualCtx*>(jd->device)->{read,decode}.
  //
  // Because the jd was already prepared with jd->device = &readCtx, swap it
  // now before jd_decomp.
  jd.device = &dual;

  // We also need infunc + outfunc that know about DualCtx.  Rather than
  // re-declaring them as new static functions (which would clutter file
  // scope), define them as local lambdas converted to function pointers via
  // a stateless lambda trick.  +[](...){...} is required to coerce captureless
  // lambda into a plain C function pointer.
  auto infuncDual = +[](JDEC* jd_, uint8_t* buff, size_t nbyte) -> size_t {
    auto* d = static_cast<DualCtx*>(jd_->device);
    if (!d || !d->read) return 0;
    auto* context = d->read;
    if (!context->file) return 0;

    if (buff == nullptr) {
      size_t skipped = 0;
      const size_t buffered = context->bufferFilled - context->bufferPos;
      const size_t fromBuf = buffered < nbyte ? buffered : nbyte;
      context->bufferPos += fromBuf;
      skipped += fromBuf;
      if (skipped < nbyte) {
        const size_t remain = nbyte - skipped;
        snapix::spi::SharedBusLock busLock;
        if (!busLock) return skipped;
        const uint64_t pos = context->file.position();
        if (!context->file.seek(pos + remain)) return skipped;
        skipped += remain;
      }
      return skipped;
    }

    size_t copied = 0;
    while (copied < nbyte) {
      if (context->bufferPos >= context->bufferFilled) {
        snapix::spi::SharedBusLock busLock;
        if (!busLock) break;
        const int readResult = context->file.read(context->buffer, JpegReadContext::kBufferSize);
        context->bufferPos = 0;
        if (readResult <= 0) {
          context->bufferFilled = 0;
          break;
        }
        context->bufferFilled = static_cast<size_t>(readResult);
      }
      const size_t available = context->bufferFilled - context->bufferPos;
      const size_t want = nbyte - copied;
      const size_t take = available < want ? available : want;
      memcpy(buff + copied, context->buffer + context->bufferPos, take);
      context->bufferPos += take;
      copied += take;
    }
    return copied;
  };

  auto outfuncDual = +[](JDEC* jd_, void* bitmap, JRECT* rect) -> int {
    auto* d = static_cast<DualCtx*>(jd_->device);
    if (!d || !d->decode) return 0;
    JpegDecodeContext* ctx = d->decode;

    if (ctx->shouldAbort && (*ctx->shouldAbort)()) {
      ctx->aborted = true;
      return 0;
    }
    if (ctx->writeError) return 0;

    const int rectTop = static_cast<int>(rect->top);
    if (rectTop > ctx->currentMcuRowTop + ctx->mcuPixelHeight - 1) {
      if (!flushMcuRow(*ctx)) return 0;
      while (rectTop > ctx->currentMcuRowTop + ctx->mcuPixelHeight - 1) {
        ctx->currentMcuRowTop += ctx->mcuPixelHeight;
      }
      memset(ctx->mcuRowBuffer,
             0,
             static_cast<size_t>(ctx->decodedWidth) * static_cast<size_t>(ctx->mcuPixelHeight));
      ctx->mcuRowsProcessed++;
      if ((ctx->mcuRowsProcessed & 0x3) == 0) taskYIELD();
    }

    const uint8_t* src = static_cast<const uint8_t*>(bitmap);
    const int rectLeft = static_cast<int>(rect->left);
    const int rectRight = static_cast<int>(rect->right);
    const int rectBottom = static_cast<int>(rect->bottom);
    const int rectW = rectRight - rectLeft + 1;

    for (int ry = rectTop; ry <= rectBottom; ry++) {
      if (ry < 0 || ry >= ctx->decodedHeight) {
        src += rectW;
        continue;
      }
      const int bufferYRow = ry - ctx->currentMcuRowTop;
      if (bufferYRow < 0 || bufferYRow >= ctx->mcuPixelHeight) {
        src += rectW;
        continue;
      }
      uint8_t* dst = ctx->mcuRowBuffer + bufferYRow * ctx->decodedWidth + rectLeft;
      const int copyW = (rectLeft + rectW > ctx->decodedWidth) ? (ctx->decodedWidth - rectLeft) : rectW;
      if (copyW > 0) memcpy(dst, src, copyW);
      src += rectW;
    }
    ctx->mcuRowDirty = true;
    return 1;
  };

  // We swapped jd.device but jd_prepare was called with the original
  // tjpgInputCb (still expecting jd->device to be JpegReadContext*).  That's
  // fine — jd_prepare has already returned by now and we're about to call
  // jd_decomp which will use jd->infunc.  Reassign jd->infunc to the dual
  // adapter:
  jd.infunc = infuncDual;

  // Run the decode.  TJpgDec sweeps MCUs in raster order, calling outfuncDual
  // once per (post-scale) MCU rectangle.
  JRESULT result = jd_decomp(&jd, outfuncDual, hwScale);

  // Flush the final partial MCU row if not already flushed.
  if (!decodeCtx.aborted && !decodeCtx.writeError && result == JDR_OK) {
    if (!flushMcuRow(decodeCtx)) {
      result = JDR_INP;  // mark as failure for cleanup path
    }
  }

  // Clean up
  if (rowAccum) delete[] rowAccum;
  if (rowCount) delete[] rowCount;
  if (atkinsonDitherer) delete atkinsonDitherer;
  if (fsDitherer) delete fsDitherer;
  if (atkinson1BitDitherer) delete atkinson1BitDitherer;
  free(mcuRowBuffer);
  free(rowBuffer);

  if (decodeCtx.aborted) {
    LOG_INF(TAG, "Abort requested during JPEG conversion");
    return false;
  }
  if (decodeCtx.writeError) {
    return false;
  }
  if (result != JDR_OK) {
    LOG_ERR(TAG, "JPEG jd_decomp failed: %d", static_cast<int>(result));
    return false;
  }

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

// Header-only peek: just enough work to extract image dimensions.  TJpgDec's
// jd_prepare reads up to the SOF marker and returns image dimensions — it
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

  // Reject progressive / arithmetic-coded JPEGs early — TJpgDec does not
  // support them either (will return JDR_FMT3).
  if (isUnsupportedJpeg(jpegFile)) {
    LOG_DBG(TAG, "peekDimensions: unsupported JPEG (progressive / arithmetic)");
    return false;
  }

  std::unique_ptr<uint8_t[]> jpegBuf(new (std::nothrow) uint8_t[JpegReadContext::kBufferSize]);
  if (!jpegBuf) return false;
  std::unique_ptr<uint8_t[]> pool(new (std::nothrow) uint8_t[TJPGD_WORKSPACE_SIZE]);
  if (!pool) return false;
  JpegReadContext readCtx = {.file = jpegFile, .buffer = jpegBuf.get(), .bufferPos = 0, .bufferFilled = 0};
  JDEC jd;
  JRESULT prep = jd_prepare(&jd, tjpgInputCb, pool.get(), TJPGD_WORKSPACE_SIZE, &readCtx);
  if (prep != JDR_OK) {
    LOG_DBG(TAG, "peekDimensions: jd_prepare failed status=%d", static_cast<int>(prep));
    return false;
  }
  if (jd.width == 0 || jd.height == 0) {
    return false;
  }
  outWidth = jd.width;
  outHeight = jd.height;
  return true;
}

// Preview decode using TJpgDec's hardware 1/8 scale (scale=3).  TJpgDec
// returns the DC value of each 8×8 block as a single decoded pixel — same
// "ant farm" trick as the picojpeg reduce=1 path, but native and 2-3× faster
// because TJpgDec does not have to walk the AC coefficients at all in 1/8
// mode (see mcu_output's "if (jd->scale != 3)" branch).
//
// On a typical 650×880 JPEG this returns in <0.5 s — the FB2 BG worker writes
// it as <id>.preview.bmp before queuing the full decode in a follow-up sweep,
// so the user sees a blurry-but-located image well before the final pixels
// arrive.  Output linear dimensions are exactly 1/8 of the original; the
// renderer upscales when drawing.
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
  std::unique_ptr<uint8_t[]> pool(new (std::nothrow) uint8_t[TJPGD_WORKSPACE_SIZE]);
  if (!pool) {
    LOG_ERR(TAG, "preview: OOM allocating TJpgDec workspace");
    return false;
  }
  JpegReadContext readCtx = {.file = jpegFile, .buffer = jpegBuf.get(), .bufferPos = 0, .bufferFilled = 0};
  JDEC jd;
  JRESULT prep = jd_prepare(&jd, tjpgInputCb, pool.get(), TJPGD_WORKSPACE_SIZE, &readCtx);
  if (prep != JDR_OK) {
    LOG_DBG(TAG, "preview: jd_prepare failed status=%d", static_cast<int>(prep));
    return false;
  }
  if (jd.width == 0 || jd.height == 0) {
    return false;
  }

  const int outW = jd.width >> 3;
  const int outH = jd.height >> 3;
  if (outW <= 0 || outH <= 0 || outW > 4096 || outH > 4096) {
    return false;
  }
  const int bytesPerRow = ((outW * 2 + 31) / 32) * 4;

  // Standard 4×4 Bayer ordered-dither matrix scaled to 0..255.  Picks a
  // different threshold per output pixel so DC values that cluster near
  // bright still produce visible black stipples instead of vanishing.
  static const uint8_t kBayer4[4][4] = {{16,  144, 48,  176},
                                        {208, 80,  240, 112},
                                        {64,  192, 32,  160},
                                        {255, 128, 224, 96}};

  // Allocate one preview output row.  The 1/8 scale means each MCU emits an
  // msx × msy rect (1×1 / 1×2 / 2×1 / 2×2 pixels).  Rectangles arrive in
  // raster order so we can process row-by-row, packing 2-bit values into the
  // current and next-row buffers as needed.
  uint8_t* outRows = static_cast<uint8_t*>(malloc(static_cast<size_t>(bytesPerRow) * 2));
  if (!outRows) {
    LOG_ERR(TAG, "preview: malloc(%d) failed", bytesPerRow * 2);
    return false;
  }
  memset(outRows, 0, static_cast<size_t>(bytesPerRow) * 2);

  writeBmpHeader2bit(bmpOut, outW, outH);

  // Streaming preview state — kept in a small struct so the C-style outfunc
  // can find it via jd->device.
  struct PreviewCtx {
    int outW;
    int outH;
    int bytesPerRow;
    uint8_t* outRows;       // capacity 2 rows
    int currentRow;         // top row in outRows
    int rowsBuffered;       // 0, 1, or 2
    Print* bmpOut;
    bool writeError;
  };
  PreviewCtx pctx = {outW, outH, bytesPerRow, outRows, 0, 0, &bmpOut, false};

  // Wrap both contexts so the input callback can still find readCtx and the
  // output callback can find pctx through the same jd->device slot.
  struct PreviewDual {
    JpegReadContext* read;
    PreviewCtx* preview;
    int yieldCounter;
  } dual = {&readCtx, &pctx, 0};

  jd.device = &dual;
  jd.infunc = +[](JDEC* jd_, uint8_t* buff, size_t nbyte) -> size_t {
    auto* d = static_cast<PreviewDual*>(jd_->device);
    if (!d || !d->read) return 0;
    auto* context = d->read;
    if (!context->file) return 0;

    if (buff == nullptr) {
      size_t skipped = 0;
      const size_t buffered = context->bufferFilled - context->bufferPos;
      const size_t fromBuf = buffered < nbyte ? buffered : nbyte;
      context->bufferPos += fromBuf;
      skipped += fromBuf;
      if (skipped < nbyte) {
        const size_t remain = nbyte - skipped;
        snapix::spi::SharedBusLock busLock;
        if (!busLock) return skipped;
        const uint64_t pos = context->file.position();
        if (!context->file.seek(pos + remain)) return skipped;
        skipped += remain;
      }
      return skipped;
    }

    size_t copied = 0;
    while (copied < nbyte) {
      if (context->bufferPos >= context->bufferFilled) {
        snapix::spi::SharedBusLock busLock;
        if (!busLock) break;
        const int readResult = context->file.read(context->buffer, JpegReadContext::kBufferSize);
        context->bufferPos = 0;
        if (readResult <= 0) {
          context->bufferFilled = 0;
          break;
        }
        context->bufferFilled = static_cast<size_t>(readResult);
      }
      const size_t available = context->bufferFilled - context->bufferPos;
      const size_t want = nbyte - copied;
      const size_t take = available < want ? available : want;
      memcpy(buff + copied, context->buffer + context->bufferPos, take);
      context->bufferPos += take;
      copied += take;
    }
    return copied;
  };

  // Output callback — packs each rect's grayscale pixels into the 2-bit row
  // buffer with Bayer threshold, flushing rows as the rect advances past
  // their bottom.
  auto outfunc = +[](JDEC* jd_, void* bitmap, JRECT* rect) -> int {
    auto* d = static_cast<PreviewDual*>(jd_->device);
    if (!d || !d->preview) return 0;
    PreviewCtx* p = d->preview;
    if (p->writeError) return 0;

    const uint8_t* src = static_cast<const uint8_t*>(bitmap);
    const int rectLeft = static_cast<int>(rect->left);
    const int rectTop = static_cast<int>(rect->top);
    const int rectRight = static_cast<int>(rect->right);
    const int rectBottom = static_cast<int>(rect->bottom);
    const int rectW = rectRight - rectLeft + 1;

    // Flush rows we've fully passed.
    while (rectTop > p->currentRow && p->rowsBuffered > 0) {
      // Write row 0 of outRows.
      snapix::spi::SharedBusLock lk;
      if (!lk || p->bmpOut->write(p->outRows, p->bytesPerRow) != static_cast<size_t>(p->bytesPerRow)) {
        p->writeError = true;
        return 0;
      }
      // Shift: row 1 becomes row 0, clear row 1.
      memcpy(p->outRows, p->outRows + p->bytesPerRow, p->bytesPerRow);
      memset(p->outRows + p->bytesPerRow, 0, p->bytesPerRow);
      p->currentRow++;
      p->rowsBuffered--;
    }

    // Each row in the rect lands either in p->outRows (rowOffset 0) or
    // p->outRows + bytesPerRow (rowOffset 1).  If a rect's top is more than
    // one row ahead of currentRow we need to skip-fill blank rows; in
    // practice this doesn't happen because rects advance by msy pixels
    // exactly.
    for (int ry = rectTop; ry <= rectBottom; ry++) {
      if (ry < 0 || ry >= p->outH) {
        src += rectW;
        continue;
      }
      const int rowOffset = ry - p->currentRow;
      if (rowOffset < 0) {
        // Rect spans into already-flushed rows — shouldn't happen given raster
        // order.  Skip defensively.
        src += rectW;
        continue;
      }
      // Grow rowsBuffered as we touch new rows.
      while (rowOffset >= p->rowsBuffered) {
        // The new row already has memset-cleared content (we only ever zero
        // outRows + bytesPerRow when shifting; the initial memset before the
        // loop covers row 0 / row 1 init).
        p->rowsBuffered++;
        if (p->rowsBuffered > 2) {
          // Should be unreachable since our buffer is 2 rows tall and a
          // single rect can span at most msy ≤ 2 rows in scale=3 mode.
          p->rowsBuffered = 2;
          break;
        }
      }
      uint8_t* dst = p->outRows + rowOffset * p->bytesPerRow;
      for (int rx = 0; rx < rectW; rx++) {
        const int outX = rectLeft + rx;
        if (outX < 0 || outX >= p->outW) continue;
        const uint8_t gray = src[rx];
        const uint8_t threshold = kBayer4[ry & 3][outX & 3];
        const uint8_t twoBit = (gray < threshold) ? 0 : 3;
        dst[outX >> 2] |= (twoBit << (6 - ((outX & 3) << 1)));
      }
      src += rectW;
    }

    d->yieldCounter++;
    if ((d->yieldCounter & 0x1F) == 0) taskYIELD();

    return 1;
  };

  JRESULT result = jd_decomp(&jd, outfunc, /*scale=*/3);

  // Flush any remaining buffered rows.
  if (!pctx.writeError && result == JDR_OK) {
    while (pctx.rowsBuffered > 0) {
      snapix::spi::SharedBusLock lk;
      if (!lk || bmpOut.write(pctx.outRows, bytesPerRow) != static_cast<size_t>(bytesPerRow)) {
        pctx.writeError = true;
        break;
      }
      memcpy(pctx.outRows, pctx.outRows + bytesPerRow, bytesPerRow);
      memset(pctx.outRows + bytesPerRow, 0, bytesPerRow);
      pctx.currentRow++;
      pctx.rowsBuffered--;
    }
  }

  free(outRows);
  if (result != JDR_OK || pctx.writeError) {
    LOG_DBG(TAG, "preview: decode err=%d writeErr=%d", static_cast<int>(result), pctx.writeError ? 1 : 0);
    return false;
  }
  LOG_INF(TAG, "preview: %dx%d decoded", outW, outH);
  return true;
}
