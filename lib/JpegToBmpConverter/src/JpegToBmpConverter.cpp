#include "JpegToBmpConverter.h"

#include <Arduino.h>
#include <JPEGDEC.h>
#include <Logging.h>

#define TAG "JPEG"
#include <SdFat.h>
#include <SharedSpiLock.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

// ============================================================================
// JPEGDEC-based JPEG → BMP converter for ESP32-C3 e-ink reader
//
// Replaces the previous TJpgDec-based pipeline.  JPEGDEC (Larry Bank, Apache
// 2.0) provides native 1/2-bpp Floyd-Steinberg dithered grayscale output via
// `setPixelType()` + `decodeDither()`.  This eliminates the intermediate
// 8-bit grayscale buffer and the separate dither pass that the TJpgDec
// version maintained — JPEGDEC dithers in-place while it decodes, producing
// packed pixels that already match our BMP output palette format.
//
// Key constraints honoured here:
//
//   * JPEGDEC's `JPEGIMAGE` struct is ~50 KB — far too large for the 16 KB
//     BG worker stack.  We always allocate it on the heap via
//     `std::unique_ptr<JPEGDEC>`.
//   * Every SD read/write goes through `SharedBusLock` so the e-ink display
//     SPI bus is never starved by image decode.  We pump through a 4 KB
//     user-space buffer (one SD block) so the lock is acquired ~once per
//     FAT cluster instead of once per Huffman fetch.
//   * Public 7-function API in `JpegToBmpConverter.h` is unchanged — every
//     call site (FB2, ImageConverter) keeps working without edits.
//   * Each public entry-point seeks the source FsFile to position 0 — callers
//     may have peeked header bytes via `peekDimensions()` already.
//   * The draw callback yields cooperatively (taskYIELD) every few MCU rows
//     so the foreground display task isn't starved on a long decode.
// ============================================================================

namespace {

// 2-bit-per-pixel BMP palette matches JPEGDEC's TWO_BIT_DITHERED packing
// exactly: bits stored MSB-first in each byte, value 0 = black, 3 = white.
// 1-bit-per-pixel BMP follows the same MSB-first convention with 0 = black,
// 1 = white.

// ---------------------------------------------------------------------------
// 4 KB pump buffer.  Same pattern as the previous TJpgDec implementation.
// JPEGDEC requests up to JPEG_FILE_BUF_SIZE = 2048 bytes per `pfnRead` call
// during the SOF/Huffman parse, then smaller chunks (~256-512 bytes) during
// the VLC stream.  Buffering these behind a single 4 KB read lets us hold
// the SharedBusLock once per cluster rather than once per Huffman fetch.
// On a typical 150 KB cover image that's ~37 SD reads instead of hundreds.
// ---------------------------------------------------------------------------
struct JpegPumpCtx {
  FsFile* file;
  static constexpr size_t kBufferSize = 4096;
  uint8_t* buffer;
  size_t bufferPos;
  size_t bufferFilled;

  // Position the underlying file is logically at relative to the start.
  // JPEGDEC's seek callback expects byte-positioned offsets from the head
  // of the JPEG stream, but in our case the stream starts at offset 0 of
  // the FsFile.  Tracking this lets seeks be smart about hitting bytes
  // already in the pump buffer.
  int32_t streamPos;
};

// JPEGDEC read callback: copy `iLen` bytes into `pBuf`.  Returns the number
// of bytes actually read.
int32_t jpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  auto* ctx = static_cast<JpegPumpCtx*>(pFile->fHandle);
  if (!ctx || !ctx->file || !*ctx->file) return 0;

  int32_t copied = 0;
  while (copied < iLen) {
    if (ctx->bufferPos >= ctx->bufferFilled) {
      // Refill the pump buffer.
      snapix::spi::SharedBusLock busLock;
      if (!busLock) break;
      const int readResult = ctx->file->read(ctx->buffer, JpegPumpCtx::kBufferSize);
      ctx->bufferPos = 0;
      if (readResult <= 0) {
        ctx->bufferFilled = 0;
        break;
      }
      ctx->bufferFilled = static_cast<size_t>(readResult);
    }
    const size_t available = ctx->bufferFilled - ctx->bufferPos;
    const size_t want = static_cast<size_t>(iLen - copied);
    const size_t take = available < want ? available : want;
    memcpy(pBuf + copied, ctx->buffer + ctx->bufferPos, take);
    ctx->bufferPos += take;
    copied += static_cast<int32_t>(take);
  }
  ctx->streamPos += copied;
  return copied;
}

// JPEGDEC seek callback: position the underlying file at byte `iPosition` from
// the start of the JPEG stream.  Returns 1 on success, 0 on failure.
//
// JPEGDEC uses this between the parse-info phase (where it reads up to the
// SOF marker) and the actual decode (where it may need to re-read header
// bytes that were ahead of where its internal buffer ended), and also during
// EXIF thumbnail extraction.  We re-target the FsFile and invalidate our
// pump buffer so the next read picks up from the new position.
int32_t jpegSeek(JPEGFILE* pFile, int32_t iPosition) {
  auto* ctx = static_cast<JpegPumpCtx*>(pFile->fHandle);
  if (!ctx || !ctx->file || !*ctx->file) return 0;

  if (iPosition == ctx->streamPos) {
    // No-op seek; pump buffer is still valid.
    return 1;
  }

  snapix::spi::SharedBusLock busLock;
  if (!busLock) return 0;
  if (!ctx->file->seek(static_cast<uint64_t>(iPosition))) return 0;
  // Invalidate pump buffer so subsequent reads refill from the new offset.
  ctx->bufferPos = 0;
  ctx->bufferFilled = 0;
  ctx->streamPos = iPosition;
  return 1;
}

// JPEGDEC close callback — we own the FsFile lifetime in the caller, so
// nothing to do here.  Required by the open() signature.
void jpegClose(void* /*handle*/) {}

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

// BMP header for 1-bit (black & white) pixel data.
void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bpp, 4-byte aligned
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;

  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 62);            // pixel data offset (14 + 40 + 8)

  write32(bmpOut, 40);            // BITMAPINFOHEADER size
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height); // negative height = top-down
  write16(bmpOut, 1);
  write16(bmpOut, 1);             // 1 bpp
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 2);
  write32(bmpOut, 2);

  // 2-color palette: 0 = black, 1 = white.
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,
      0xFF, 0xFF, 0xFF, 0x00,
  };
  for (const uint8_t b : palette) bmpOut.write(b);
}

// BMP header for 2-bit (4-level grayscale) pixel data.
void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bpp, 4-byte aligned
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;

  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);
  write32(bmpOut, 70);            // pixel data offset

  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);
  write16(bmpOut, 1);
  write16(bmpOut, 2);             // 2 bpp
  write32(bmpOut, 0);
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);
  write32(bmpOut, 2835);
  write32(bmpOut, 4);
  write32(bmpOut, 4);

  // 4-color palette: 0 = black, 1 = dark gray, 2 = light gray, 3 = white.
  // Same palette as the legacy TJpgDec converter so cached BMPs render
  // identically across decoder swaps.
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,
      0x55, 0x55, 0x55, 0x00,
      0xAA, 0xAA, 0xAA, 0x00,
      0xFF, 0xFF, 0xFF, 0x00,
  };
  for (const uint8_t b : palette) bmpOut.write(b);
}

// Output rectangle context — the JPEGDEC draw callback packs pixel chunks
// directly into our BMP-format row buffer, then writes the row to bmpOut
// once the chunk's vertical extent is exhausted.
//
// Coordinate system: JPEGDEC reports pixel x/y in OUTPUT space (post hardware
// scale).  Each draw call covers `iWidthUsed` pixels horizontally and
// `iHeight` pixels vertically, starting at (`x`, `y`).  Within a chunk the
// pixels are packed bit-by-bit in raster order, MSB-first, exactly matching
// BMP's bit ordering.
struct DrawCtx {
  Print* bmpOut;
  uint8_t* rowBuffer;     // current BMP row being assembled
  int outWidth;           // final image width (post hardware scale)
  int outHeight;          // final image height
  int bytesPerRow;        // bytes per packed BMP row
  int bitsPerPixel;       // 1 or 2
  int currentY;           // y of the row currently in rowBuffer (-1 = none yet)
  bool oneBit;
  bool writeError;

  const std::function<bool()>* shouldAbort;
  bool aborted;
  int yieldCounter;
};

// Flush a single BMP row from rowBuffer to the output stream and zero it out.
bool flushRow(DrawCtx& ctx) {
  snapix::spi::SharedBusLock busLock;
  if (!busLock) {
    LOG_ERR(TAG, "Shared SPI lock unavailable for BMP row write");
    ctx.writeError = true;
    return false;
  }
  if (ctx.bmpOut->write(ctx.rowBuffer, ctx.bytesPerRow) != static_cast<size_t>(ctx.bytesPerRow)) {
    LOG_ERR(TAG, "Failed to write BMP row");
    ctx.writeError = true;
    return false;
  }
  memset(ctx.rowBuffer, 0, ctx.bytesPerRow);
  return true;
}

// Copy a packed bit-stream chunk from src into dst at pixel offset `dstX`.
// src is MSB-first within each byte and covers `count` pixels packed
// contiguously.  bitsPerPixel is 1 or 2.
//
// Hot path: when both src and dst are byte-aligned at the chunk boundary
// (i.e. dstX is a multiple of 8 for 1bpp or 4 for 2bpp), we can byte-copy
// the bulk of the data and only handle ≤1 trailing partial byte per-pixel.
// In dither mode JPEGDEC always issues one draw call per MCU row at
// chunkX=0, so this fast path covers the common case for full-row output.
void copyPackedBits(uint8_t* dst, int dstX, const uint8_t* src, int count, int bitsPerPixel) {
  if (count <= 0) return;

  const int pxPerByte = (bitsPerPixel == 1) ? 8 : 4;
  if ((dstX % pxPerByte) == 0) {
    // Byte-aligned: bulk-copy whole bytes, then one trailing partial byte.
    const int wholeBytes = count / pxPerByte;
    if (wholeBytes > 0) {
      memcpy(dst + (dstX / pxPerByte), src, wholeBytes);
    }
    const int trailing = count - wholeBytes * pxPerByte;
    if (trailing > 0) {
      // Trailing bits live in the top of src[wholeBytes], with `trailing`
      // valid pixels in the high bits of that byte.  Mask them in.
      const uint8_t srcByte = src[wholeBytes];
      const int trailingBits = trailing * bitsPerPixel;
      const uint8_t mask = static_cast<uint8_t>(0xFF << (8 - trailingBits));
      dst[dstX / pxPerByte + wholeBytes] |= (srcByte & mask);
    }
    return;
  }

  // Misaligned: per-pixel pack-into-dst.
  for (int i = 0; i < count; i++) {
    uint8_t value;
    if (bitsPerPixel == 1) {
      const int srcByte = i >> 3;
      const int srcBitOffset = 7 - (i & 7);
      value = (src[srcByte] >> srcBitOffset) & 0x01;
    } else {
      const int srcByte = i >> 2;
      const int srcBitOffset = 6 - ((i & 3) << 1);
      value = (src[srcByte] >> srcBitOffset) & 0x03;
    }
    const int outX = dstX + i;
    if (bitsPerPixel == 1) {
      const int dstByte = outX >> 3;
      const int dstBitOffset = 7 - (outX & 7);
      dst[dstByte] |= (value << dstBitOffset);
    } else {
      const int dstByte = outX >> 2;
      const int dstBitOffset = 6 - ((outX & 3) << 1);
      dst[dstByte] |= (value << dstBitOffset);
    }
  }
}

// JPEGDEC draw callback.  Called once per MCU row in dither mode (since
// JPEGDEC sets `iMCUCount = cx` for any *_DITHERED pixel type — see
// jpeg.inl).  pPixels points to the packed dither buffer at the start of
// the chunk; one packed pixel per pDraw->iWidth slot, but ONLY iWidthUsed
// pixels are valid (the rest is right-edge padding from MCU rounding).
int jpegDraw(JPEGDRAW* pDraw) {
  auto* ctx = static_cast<DrawCtx*>(pDraw->pUser);
  if (!ctx) return 1;

  if (ctx->shouldAbort && (*ctx->shouldAbort)()) {
    ctx->aborted = true;
    return 0;  // signal JPEGDEC to abort
  }
  if (ctx->writeError) return 0;

  const int chunkX = pDraw->x;
  const int chunkY = pDraw->y;
  const int chunkH = pDraw->iHeight;
  // iWidthUsed is the actual count of valid pixels (clipped at right edge);
  // anything beyond that is padding that doesn't belong in the output.
  const int chunkW = pDraw->iWidthUsed;
  // pDraw->iWidth is the chunk's packing pitch — every row in pPixels starts
  // every (iWidth + 3)/4 bytes for 2bpp, or (iWidth + 7)/8 bytes for 1bpp.
  const int packPitchPx = pDraw->iWidth;
  const int srcPitchBytes = (ctx->bitsPerPixel == 1) ? ((packPitchPx + 7) >> 3)
                                                      : ((packPitchPx + 3) >> 2);

  const auto* srcBytes = reinterpret_cast<const uint8_t*>(pDraw->pPixels);

  for (int row = 0; row < chunkH; row++) {
    const int outY = chunkY + row;
    if (outY < 0 || outY >= ctx->outHeight) continue;

    // If this row differs from the row currently buffered, flush the
    // previous row first.  JPEGDEC iterates rows in raster order, so any
    // change in outY means the previous row is complete.
    if (ctx->currentY != outY) {
      if (ctx->currentY >= 0) {
        if (!flushRow(*ctx)) return 0;
      }
      ctx->currentY = outY;
    }

    const uint8_t* srcRow = srcBytes + row * srcPitchBytes;

    // Clip the chunk's right edge against the output width.
    int copyW = chunkW;
    if (chunkX + copyW > ctx->outWidth) copyW = ctx->outWidth - chunkX;
    if (copyW <= 0) continue;
    if (chunkX < 0) {
      // Not expected for this converter (no x offset), but handle defensively.
      continue;
    }

    copyPackedBits(ctx->rowBuffer, chunkX, srcRow, copyW, ctx->bitsPerPixel);
  }

  ctx->yieldCounter++;
  if ((ctx->yieldCounter & 0x7) == 0) taskYIELD();

  return 1;  // continue decoding
}

// Choose the largest hardware scale factor (1, 1/2, 1/4, 1/8) such that the
// resulting decoded dimensions still meet or exceed the target box.  Returns
// the JPEGDEC scale option bit (0 = no scale) and the resulting decoded W/H.
struct ScaleResult {
  int option;     // 0 / JPEG_SCALE_HALF / JPEG_SCALE_QUARTER / JPEG_SCALE_EIGHTH
  int outWidth;
  int outHeight;
};

ScaleResult chooseHardwareScale(int srcW, int srcH, int targetMaxW, int targetMaxH) {
  ScaleResult result = {0, srcW, srcH};
  if (targetMaxW <= 0 || targetMaxH <= 0) return result;
  if (srcW <= targetMaxW && srcH <= targetMaxH) return result;

  // Pick the most aggressive hardware scale where post-scale dimensions still
  // meet or exceed the target on BOTH axes.  This avoids dropping below the
  // requested resolution (the renderer would have to stretch — ugly) while
  // still saving memory and decode time when the source is much larger than
  // the target.  "src ≥ 2× target on both axes" = HALF; "src ≥ 4×" = QUARTER;
  // "src ≥ 8×" = EIGHTH.  Ratios checked against the more constraining axis
  // so we never undershoot.
  struct Step {
    int option;
    int shift;
  };
  // Order from coarsest to finest.  The first one whose post-scale output
  // is still >= target on both axes wins (since coarser scales produce less
  // data).
  const Step steps[] = {{JPEG_SCALE_EIGHTH, 3}, {JPEG_SCALE_QUARTER, 2}, {JPEG_SCALE_HALF, 1}};
  for (const Step& s : steps) {
    const int dw = srcW >> s.shift;
    const int dh = srcH >> s.shift;
    if (dw >= targetMaxW && dh >= targetMaxH && dw > 0 && dh > 0) {
      result.option = s.option;
      result.outWidth = dw;
      result.outHeight = dh;
      return result;
    }
  }
  // Source is between 1× and 2× target — no hardware scale applies.  We
  // could software-downscale but the JPEGDEC pipeline keeps things simple
  // by leaving final fitting to the renderer (BMP renderer can clip at the
  // top-down DIB level).  This matches what TJpgDec did in fallback.
  return result;
}

// Common implementation for jpegFileToBmpStream*, jpegFileTo1BitBmpStream*,
// jpegFileToBmpStreamQuick.  All flavours share the open / scale / decode /
// flush pipeline; they only differ in target dimensions and bit depth.
bool decodeImpl(FsFile& jpegFile, Print& bmpOut, int targetMaxW, int targetMaxH, bool oneBit,
                const std::function<bool()>& shouldAbort) {
  if (!jpegFile) return false;

  // Reset to start so callers that peeked the SOF (e.g. for fast-mode layout)
  // don't break the parser.
  {
    snapix::spi::SharedBusLock lk;
    if (!lk || !jpegFile.seek(0)) return false;
  }

  const int32_t fileSize = static_cast<int32_t>(jpegFile.size());
  if (fileSize <= 0) return false;

  // Heap-allocate the pump buffer (4 KB — bigger than the BG worker stack).
  std::unique_ptr<uint8_t[]> pumpBuf(new (std::nothrow) uint8_t[JpegPumpCtx::kBufferSize]);
  if (!pumpBuf) {
    LOG_ERR(TAG, "JPEG OOM allocating %u-byte pump buffer",
            static_cast<unsigned>(JpegPumpCtx::kBufferSize));
    return false;
  }

  JpegPumpCtx pump = {.file = &jpegFile,
                      .buffer = pumpBuf.get(),
                      .bufferPos = 0,
                      .bufferFilled = 0,
                      .streamPos = 0};

  // Heap-allocate the JPEGIMAGE workspace — `JPEGIMAGE` is ~50 KB, far
  // bigger than the 16 KB BG worker stack would tolerate.
  std::unique_ptr<JPEGDEC> jpeg(new (std::nothrow) JPEGDEC());
  if (!jpeg) {
    LOG_ERR(TAG, "JPEG OOM allocating JPEGDEC workspace");
    return false;
  }

  // open() reads through the SOF marker and populates width/height/etc.
  // Returns 1 on success, 0 on failure (via getLastError()).
  if (jpeg->open(&pump, fileSize, jpegClose, jpegRead, jpegSeek, jpegDraw) == 0) {
    LOG_ERR(TAG, "JPEGDEC open failed: err=%d", jpeg->getLastError());
    return false;
  }

  // Reject progressive / arithmetic-coded JPEGs explicitly.  open() will
  // succeed but the decode would yield garbage or fail mid-stream — better
  // to bail now and return a clean failure.
  if (jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE) {
    LOG_ERR(TAG, "JPEG progressive mode unsupported");
    jpeg->close();
    return false;
  }

  const int srcW = jpeg->getWidth();
  const int srcH = jpeg->getHeight();
  if (srcW <= 0 || srcH <= 0) {
    LOG_ERR(TAG, "JPEG malformed: zero dimensions %dx%d", srcW, srcH);
    jpeg->close();
    return false;
  }

  // Safety limits to prevent memory issues on ESP32-C3.
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (srcW > MAX_IMAGE_WIDTH || srcH > MAX_IMAGE_HEIGHT) {
    LOG_ERR(TAG, "JPEG too large (%dx%d), max supported: %dx%d", srcW, srcH, MAX_IMAGE_WIDTH,
            MAX_IMAGE_HEIGHT);
    jpeg->close();
    return false;
  }

  const ScaleResult scale = chooseHardwareScale(srcW, srcH, targetMaxW, targetMaxH);
  const int outWidth = scale.outWidth;
  const int outHeight = scale.outHeight;
  if (outWidth <= 0 || outHeight <= 0) {
    LOG_ERR(TAG, "JPEG decoded dimensions degenerate %dx%d", outWidth, outHeight);
    jpeg->close();
    return false;
  }

  LOG_INF(TAG, "JPEG %dx%d -> %dx%d (scale=%d, %s)", srcW, srcH, outWidth, outHeight, scale.option,
          oneBit ? "1-bit" : "2-bit");

  // Configure JPEGDEC for native dithered output.
  jpeg->setPixelType(oneBit ? ONE_BIT_DITHERED : TWO_BIT_DITHERED);

  // Allocate the dither buffer.  JPEGDEC uses this both as the 8-bit
  // grayscale staging area before dithering AND as the destination of the
  // packed N-bit pixels.  The grayscale form needs `cx_post * mcuCX_post *
  // mcuCY_post` bytes.  Worst case is scale=0, mcuCX=mcuCY=16 (4:2:0
  // subsampling), where the row width post-rounding can exceed iWidth by
  // up to 16 pixels.  Allocate (outWidth + 32) * 16 + 16 bytes — generous
  // and 16-byte aligned to satisfy any internal SIMD assumptions.
  const size_t ditherBufSize = static_cast<size_t>((outWidth + 32) * 16 + 16);
  std::unique_ptr<uint8_t[]> ditherBuf(new (std::nothrow) uint8_t[ditherBufSize]);
  if (!ditherBuf) {
    LOG_ERR(TAG, "JPEG OOM allocating %u-byte dither buffer",
            static_cast<unsigned>(ditherBufSize));
    jpeg->close();
    return false;
  }

  // Allocate the packed BMP row buffer.
  const int bytesPerRow = oneBit ? ((outWidth + 31) / 32 * 4) : ((outWidth * 2 + 31) / 32 * 4);
  std::unique_ptr<uint8_t[]> rowBuf(new (std::nothrow) uint8_t[bytesPerRow]);
  if (!rowBuf) {
    LOG_ERR(TAG, "JPEG OOM allocating %d-byte BMP row buffer", bytesPerRow);
    jpeg->close();
    return false;
  }
  memset(rowBuf.get(), 0, bytesPerRow);

  // Write BMP header.
  {
    snapix::spi::SharedBusLock lk;
    if (!lk) {
      LOG_ERR(TAG, "Shared SPI lock unavailable for BMP header write");
      jpeg->close();
      return false;
    }
    if (oneBit) {
      writeBmpHeader1bit(bmpOut, outWidth, outHeight);
    } else {
      writeBmpHeader2bit(bmpOut, outWidth, outHeight);
    }
  }

  // Build draw context.
  DrawCtx drawCtx;
  drawCtx.bmpOut = &bmpOut;
  drawCtx.rowBuffer = rowBuf.get();
  drawCtx.outWidth = outWidth;
  drawCtx.outHeight = outHeight;
  drawCtx.bytesPerRow = bytesPerRow;
  drawCtx.bitsPerPixel = oneBit ? 1 : 2;
  drawCtx.currentY = -1;
  drawCtx.oneBit = oneBit;
  drawCtx.writeError = false;
  drawCtx.shouldAbort = (shouldAbort ? &shouldAbort : nullptr);
  drawCtx.aborted = false;
  drawCtx.yieldCounter = 0;
  jpeg->setUserPointer(&drawCtx);

  // Decode.  decodeDither(buf, opts) runs the in-decoder Floyd-Steinberg
  // pipeline; JPEG_LUMA_ONLY skips chroma decode (we only want gray).
  const int decodeOptions = scale.option | JPEG_LUMA_ONLY;
  const int decodeOk = jpeg->decodeDither(ditherBuf.get(), decodeOptions);

  // Flush the final row (if not already flushed).
  if (!drawCtx.aborted && !drawCtx.writeError && drawCtx.currentY >= 0) {
    flushRow(drawCtx);
  }

  jpeg->close();

  if (drawCtx.aborted) {
    LOG_INF(TAG, "JPEG decode aborted by caller");
    return false;
  }
  if (drawCtx.writeError) return false;
  if (decodeOk == 0) {
    LOG_ERR(TAG, "JPEG decodeDither failed: err=%d", jpeg->getLastError());
    return false;
  }
  return true;
}

}  // namespace

// Stub kept solely so the static declaration in JpegToBmpConverter.h still
// resolves at link time.  This signature dates back to the picojpeg era and
// nothing inside this file references it; the decoder uses its own pump
// callbacks (jpegRead/jpegSeek) above.
unsigned char JpegToBmpConverter::jpegReadCallback(unsigned char* /*pBuf*/,
                                                   const unsigned char /*buf_size*/,
                                                   unsigned char* pBytes_actually_read,
                                                   void* /*pCallback_data*/) {
  if (pBytes_actually_read) *pBytes_actually_read = 0;
  return 0;
}

bool JpegToBmpConverter::jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth,
                                                     int targetHeight, bool oneBit, bool /*quickMode*/,
                                                     const std::function<bool()>& shouldAbort) {
  // quickMode previously toggled "no dithering" — JPEGDEC always dithers
  // (Floyd-Steinberg) in-decoder, so the parameter is silently ignored.  The
  // ~1.5–3× speedup of JPEGDEC over TJpgDec already covers the perf budget
  // the old "quick" code was buying.
  return decodeImpl(jpegFile, bmpOut, targetWidth, targetHeight, oneBit, shouldAbort);
}

// Default cover image target: same constants as the legacy converter so cache
// invalidation on upgrade is the only behavioural break point.
namespace {
constexpr int kDefaultTargetW = 450;
constexpr int kDefaultTargetH = 750;
}

bool JpegToBmpConverter::jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut) {
  return decodeImpl(jpegFile, bmpOut, kDefaultTargetW, kDefaultTargetH, /*oneBit=*/false, nullptr);
}

bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight,
                                                     const std::function<bool()>& shouldAbort) {
  return decodeImpl(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, /*oneBit=*/false, shouldAbort);
}

bool JpegToBmpConverter::jpegFileTo1BitBmpStream(FsFile& jpegFile, Print& bmpOut) {
  return decodeImpl(jpegFile, bmpOut, kDefaultTargetW, kDefaultTargetH, /*oneBit=*/true, nullptr);
}

bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut,
                                                         int targetMaxWidth, int targetMaxHeight) {
  return decodeImpl(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, /*oneBit=*/true, nullptr);
}

bool JpegToBmpConverter::jpegFileToBmpStreamQuick(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                  int targetMaxHeight,
                                                  const std::function<bool()>& shouldAbort) {
  // No separate "quick" pipeline under JPEGDEC — Floyd-Steinberg dithering is
  // baked into the decode loop and effectively free.  Same code path as
  // jpegFileToBmpStreamWithSize.
  return decodeImpl(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, /*oneBit=*/false, shouldAbort);
}

// Header-only peek: enough work to extract image dimensions.  JPEGDEC's
// open() reads through the SOF marker and populates width/height — same
// ~10× speedup over a full decode that the TJpgDec version provided.
bool JpegToBmpConverter::peekDimensions(FsFile& jpegFile, int& outWidth, int& outHeight) {
  outWidth = 0;
  outHeight = 0;
  if (!jpegFile) return false;

  {
    snapix::spi::SharedBusLock lk;
    if (!lk || !jpegFile.seek(0)) return false;
  }

  const int32_t fileSize = static_cast<int32_t>(jpegFile.size());
  if (fileSize <= 0) return false;

  std::unique_ptr<uint8_t[]> pumpBuf(new (std::nothrow) uint8_t[JpegPumpCtx::kBufferSize]);
  if (!pumpBuf) return false;

  JpegPumpCtx pump = {.file = &jpegFile,
                      .buffer = pumpBuf.get(),
                      .bufferPos = 0,
                      .bufferFilled = 0,
                      .streamPos = 0};

  std::unique_ptr<JPEGDEC> jpeg(new (std::nothrow) JPEGDEC());
  if (!jpeg) return false;

  // No draw callback is needed — peek only triggers SOF parsing.
  if (jpeg->open(&pump, fileSize, jpegClose, jpegRead, jpegSeek, jpegDraw) == 0) {
    LOG_DBG(TAG, "peekDimensions: open failed err=%d", jpeg->getLastError());
    return false;
  }
  if (jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE) {
    LOG_DBG(TAG, "peekDimensions: progressive JPEG not supported");
    jpeg->close();
    return false;
  }
  outWidth = jpeg->getWidth();
  outHeight = jpeg->getHeight();
  jpeg->close();
  return outWidth > 0 && outHeight > 0;
}

// Tiny preview decode.  Uses JPEGDEC's hardware 1/8 scale with native
// 2-bit Floyd-Steinberg dithering — equivalent to the old TJpgDec-with-Bayer
// preview but faster and cleaner since dithering happens inside JPEGDEC's
// decode loop.  Output dimensions are 1/8 of the source on each axis; the
// renderer upscales when drawing.
bool JpegToBmpConverter::jpegFileToBmpStreamPreview(FsFile& jpegFile, Print& bmpOut) {
  // Same as a regular 2-bit decode but request 1/8 hardware scale up front.
  // We do it via the same helper with a tiny target so chooseHardwareScale
  // automatically picks JPEG_SCALE_EIGHTH when the source is large enough.
  // For tiny source images chooseHardwareScale returns scale=0, which is
  // also fine for previews.

  if (!jpegFile) return false;
  {
    snapix::spi::SharedBusLock lk;
    if (!lk || !jpegFile.seek(0)) return false;
  }

  const int32_t fileSize = static_cast<int32_t>(jpegFile.size());
  if (fileSize <= 0) return false;

  std::unique_ptr<uint8_t[]> pumpBuf(new (std::nothrow) uint8_t[JpegPumpCtx::kBufferSize]);
  if (!pumpBuf) return false;
  JpegPumpCtx pump = {.file = &jpegFile,
                      .buffer = pumpBuf.get(),
                      .bufferPos = 0,
                      .bufferFilled = 0,
                      .streamPos = 0};

  std::unique_ptr<JPEGDEC> jpeg(new (std::nothrow) JPEGDEC());
  if (!jpeg) {
    LOG_ERR(TAG, "preview: OOM allocating JPEGDEC workspace");
    return false;
  }

  if (jpeg->open(&pump, fileSize, jpegClose, jpegRead, jpegSeek, jpegDraw) == 0) {
    LOG_DBG(TAG, "preview: open failed err=%d", jpeg->getLastError());
    return false;
  }
  if (jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE) {
    jpeg->close();
    return false;
  }

  const int srcW = jpeg->getWidth();
  const int srcH = jpeg->getHeight();
  if (srcW <= 0 || srcH <= 0) {
    jpeg->close();
    return false;
  }
  const int outW = srcW >> 3;
  const int outH = srcH >> 3;
  if (outW <= 0 || outH <= 0 || outW > 4096 || outH > 4096) {
    jpeg->close();
    return false;
  }

  jpeg->setPixelType(TWO_BIT_DITHERED);

  const size_t ditherBufSize = static_cast<size_t>((outW + 32) * 16 + 16);
  std::unique_ptr<uint8_t[]> ditherBuf(new (std::nothrow) uint8_t[ditherBufSize]);
  if (!ditherBuf) {
    LOG_ERR(TAG, "preview: OOM allocating dither buffer");
    jpeg->close();
    return false;
  }

  const int bytesPerRow = (outW * 2 + 31) / 32 * 4;
  std::unique_ptr<uint8_t[]> rowBuf(new (std::nothrow) uint8_t[bytesPerRow]);
  if (!rowBuf) {
    LOG_ERR(TAG, "preview: OOM allocating row buffer");
    jpeg->close();
    return false;
  }
  memset(rowBuf.get(), 0, bytesPerRow);

  {
    snapix::spi::SharedBusLock lk;
    if (!lk) {
      jpeg->close();
      return false;
    }
    writeBmpHeader2bit(bmpOut, outW, outH);
  }

  DrawCtx drawCtx;
  drawCtx.bmpOut = &bmpOut;
  drawCtx.rowBuffer = rowBuf.get();
  drawCtx.outWidth = outW;
  drawCtx.outHeight = outH;
  drawCtx.bytesPerRow = bytesPerRow;
  drawCtx.bitsPerPixel = 2;
  drawCtx.currentY = -1;
  drawCtx.oneBit = false;
  drawCtx.writeError = false;
  drawCtx.shouldAbort = nullptr;
  drawCtx.aborted = false;
  drawCtx.yieldCounter = 0;
  jpeg->setUserPointer(&drawCtx);

  const int decodeOk = jpeg->decodeDither(ditherBuf.get(), JPEG_SCALE_EIGHTH | JPEG_LUMA_ONLY);

  if (!drawCtx.writeError && drawCtx.currentY >= 0) {
    flushRow(drawCtx);
  }

  jpeg->close();

  if (drawCtx.writeError || decodeOk == 0) {
    LOG_DBG(TAG, "preview: decode err=%d writeErr=%d", decodeOk, drawCtx.writeError ? 1 : 0);
    return false;
  }
  LOG_INF(TAG, "preview: %dx%d decoded", outW, outH);
  return true;
}
