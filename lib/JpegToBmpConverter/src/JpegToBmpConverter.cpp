#include "JpegToBmpConverter.h"

#include <Arduino.h>
#include <JPEGDEC.h>
#include <Logging.h>

#define TAG "JPEG"
#include <SdFat.h>
#include <SharedSpiLock.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

// ============================================================================
// JPEGDEC-based JPEG → 1-bit BMP converter for ESP32-C3 e-ink reader.
//
// Pipeline:
//   1. JPEGDEC parses the SOF and reports source dimensions.
//   2. We pick the largest hardware scale (1, 1/2, 1/4, 1/8) that leaves the
//      decoded image still ≥ the requested target box.  This guarantees the
//      software step only ever DOWNscales (never upscales) — "always
//      interpolating downward".
//   3. JPEGDEC decodes into EIGHT_BIT_GRAYSCALE pixels which we accumulate
//      into a per-MCU-row gray band buffer.
//   4. As source rows complete, we area-average them down into a single
//      target-row of grayscale (vertical downscale) then area-average
//      horizontally to the target width (so a 534×628 source emits a
//      452×532 grayscale row).
//   5. Floyd-Steinberg dithering across each target-width gray row produces
//      a packed 1-bpp row that is written immediately to the output BMP.
//
// Why 1-bit dithered output:
//   * The e-paper renderer's BW path already collapses 2-bit values < 3 to
//     "black" anyway, so the old 4-level palette wasted half its storage on
//     levels that never reached the panel.
//   * Saving the BMP at exact target dims means drawBitmap can stop scaling
//     entirely — direct row-aligned bit copy with rotation only.
//   * Per-image: 452×532 1-bpp ≈ 30 KB vs. 534×628 2-bpp ≈ 83 KB on disk,
//     and the per-page render cost drops from 2-7 s to fractional.
//
// Constraints honoured:
//   * Every SD read/write goes through `SharedBusLock` so the e-ink display
//     SPI bus is never starved by image decode.  We pump through a 4 KB
//     user-space buffer (one SD block) so the lock is acquired ~once per
//     FAT cluster instead of once per Huffman fetch.
//   * The JPEGDEC workspace (~50 KB) and our own scratch buffers are heap
//     allocated — never on the BG worker stack.
//   * Public 7-function API in `JpegToBmpConverter.h` is unchanged.
//   * Each public entry point seeks the source FsFile to position 0 — callers
//     may have peeked header bytes via `peekDimensions()` already.
//   * The draw callback yields cooperatively (taskYIELD) every few MCU rows
//     so the foreground display task isn't starved on a long decode.
//   * `peekDimensions()` returns SOURCE JPEG dims (not post-downscale dims):
//     `Fb2::cacheImage` does its own scaledFit() based on those.
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// 4 KB SD pump buffer.  JPEGDEC requests up to JPEG_FILE_BUF_SIZE = 2048 bytes
// per `pfnRead` call during the SOF/Huffman parse, then smaller chunks (~256-
// 512 bytes) during the VLC stream.  Buffering these behind a single 4 KB
// read lets us hold the SharedBusLock once per cluster rather than once per
// Huffman fetch.  On a typical 150 KB cover image that's ~37 SD reads instead
// of hundreds.
// ---------------------------------------------------------------------------
struct JpegPumpCtx {
  FsFile* file;
  static constexpr size_t kBufferSize = 4096;
  uint8_t* buffer;
  size_t bufferPos;
  size_t bufferFilled;
  // Position the underlying file is logically at relative to the start of
  // the JPEG stream.  Tracking this lets seeks be smart about hitting bytes
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
int32_t jpegSeek(JPEGFILE* pFile, int32_t iPosition) {
  auto* ctx = static_cast<JpegPumpCtx*>(pFile->fHandle);
  if (!ctx || !ctx->file || !*ctx->file) return 0;

  if (iPosition == ctx->streamPos) return 1;  // No-op; pump buffer still valid.

  snapix::spi::SharedBusLock busLock;
  if (!busLock) return 0;
  if (!ctx->file->seek(static_cast<uint64_t>(iPosition))) return 0;
  ctx->bufferPos = 0;
  ctx->bufferFilled = 0;
  ctx->streamPos = iPosition;
  return 1;
}

// JPEGDEC close callback — we own the FsFile lifetime in the caller.
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

// 1-bit BMP header (palette: 0=black, 1=white, MSB-first packing).
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

  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,
      0xFF, 0xFF, 0xFF, 0x00,
  };
  for (const uint8_t b : palette) bmpOut.write(b);
}

// ---------------------------------------------------------------------------
// Hardware-scale heuristic.  Picks the largest JPEGDEC scale (0=1, 1=1/2,
// 2=1/4, 3=1/8) such that the decoded dims still cover `maxW × maxH`.  This
// guarantees the software downscale step never has to upscale.
// ---------------------------------------------------------------------------
struct ScaleResult {
  int option;     // JPEG_SCALE_* bit, or 0 for native
  int decodedW;   // post-hardware-scale source width
  int decodedH;   // post-hardware-scale source height
};

ScaleResult chooseHardwareScale(int srcW, int srcH, int maxW, int maxH) {
  ScaleResult result = {0, srcW, srcH};
  if (maxW <= 0 || maxH <= 0) return result;

  // scale=0 => 1/1, scale=1 => 1/2, scale=2 => 1/4, scale=3 => 1/8.
  // We want the largest `scale` such that (srcW >> scale) >= maxW and
  // (srcH >> scale) >= maxH.  Cap at 3.
  int scale = 0;
  while (scale < 3) {
    const int nextW = srcW >> (scale + 1);
    const int nextH = srcH >> (scale + 1);
    if (nextW >= maxW && nextH >= maxH && nextW > 0 && nextH > 0) {
      ++scale;
    } else {
      break;
    }
  }

  static constexpr int kOptions[4] = {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH};
  result.option = kOptions[scale];
  result.decodedW = srcW >> scale;
  result.decodedH = srcH >> scale;
  if (result.decodedW <= 0) result.decodedW = 1;
  if (result.decodedH <= 0) result.decodedH = 1;
  return result;
}

// ---------------------------------------------------------------------------
// Decode context: shared between the JPEGDEC draw callback and the per-band
// downscale + dither pipeline.
//
// The draw callback's job is purely to copy 8-bit grayscale pixels into the
// MCU-row band buffer at the right offset.  Once a full MCU band is in the
// buffer (i.e. the next draw moves to a new chunkY), processBand() is called
// to vertically area-average those rows down into target-rows of grayscale,
// which are then horizontally area-averaged + Floyd-Steinberg dithered into
// 1-bit packed BMP rows and written out.
// ---------------------------------------------------------------------------
struct DecodeCtx {
  Print* bmpOut = nullptr;

  int decodedW = 0;       // post-hardware-scale source width
  int decodedH = 0;       // post-hardware-scale source height
  int targetW = 0;        // final BMP width
  int targetH = 0;        // final BMP height
  int bytesPerRow = 0;    // packed 1-bpp row size

  // 8-bit gray accumulator for one MCU band (decodedW × mcuCY_post bytes).
  uint8_t* mcuBand = nullptr;
  int mcuBandHeight = 0;  // number of valid source rows currently in mcuBand
  int mcuBandY = -1;      // chunkY of the band currently being filled

  // Per target-row vertical accumulator: sum of source-row gray values that
  // map to the current target row, plus count.  When the target-row index
  // advances, we finalize this row.
  uint32_t* vertSum = nullptr;     // length decodedW
  int vertSumCount = 0;
  int currentTargetY = -1;         // target row currently being accumulated

  // Per output-row scratch.
  uint8_t* targetGrayRow = nullptr;  // length targetW (8-bit gray pre-dither)
  uint8_t* outRow = nullptr;         // length bytesPerRow (1-bpp packed)

  // Floyd-Steinberg error rows (extra slot on each end for ±1 spillover).
  int16_t* errCur = nullptr;       // length targetW + 2 (index 1..targetW)
  int16_t* errNext = nullptr;      // length targetW + 2

  // Cached precomputed source-X start positions for horizontal area-average.
  // srcXStart[outX] gives the inclusive lower bound; srcXStart[targetW] gives
  // the exclusive upper bound for the last column.
  int* srcXStart = nullptr;        // length targetW + 1

  const std::function<bool()>* shouldAbort = nullptr;
  bool aborted = false;
  bool writeError = false;
  int yieldCounter = 0;
};

// Vertical mapping: which output target-row does source-row `srcY` belong to.
inline int sourceYToTargetY(int srcY, int decodedH, int targetH) {
  // (srcY * targetH) / decodedH, rounded down.  Equivalent to a fixed-point
  // 16.16 mapping clipped to [0, targetH-1].
  if (decodedH <= 0) return 0;
  int t = static_cast<int>((static_cast<int64_t>(srcY) * targetH) / decodedH);
  if (t < 0) t = 0;
  if (t > targetH - 1) t = targetH - 1;
  return t;
}

// Horizontal area-average from a `decodedW` 8-bit gray row to a `targetW`
// 8-bit gray row.  Uses ctx.srcXStart[] precomputed to avoid per-pixel
// 64-bit math.  When the source spans are not byte-aligned we sum
// rectangular areas of pixels.
void areaAverageHoriz(const uint8_t* srcGray, int decodedW, uint8_t* dstGray, int targetW,
                      const int* srcXStart) {
  for (int outX = 0; outX < targetW; ++outX) {
    int sx0 = srcXStart[outX];
    int sx1 = srcXStart[outX + 1];
    if (sx0 < 0) sx0 = 0;
    if (sx1 > decodedW) sx1 = decodedW;
    if (sx1 <= sx0) sx1 = sx0 + 1;
    if (sx1 > decodedW) sx1 = decodedW;

    uint32_t sum = 0;
    int n = 0;
    for (int x = sx0; x < sx1; ++x) {
      sum += srcGray[x];
      ++n;
    }
    const uint32_t avg = (n > 0) ? (sum / static_cast<uint32_t>(n)) : 0u;
    dstGray[outX] = static_cast<uint8_t>(avg > 255 ? 255 : avg);
  }
}

// Floyd-Steinberg dither one row of 8-bit gray (length targetW) into a 1-bpp
// packed BMP row (length bytesPerRow).  Updates the error rows in place.
void floydSteinbergRow(DecodeCtx& ctx) {
  const int targetW = ctx.targetW;
  uint8_t* outRow = ctx.outRow;
  memset(outRow, 0, ctx.bytesPerRow);

  // errCur / errNext are sized [targetW + 2].  Index 0 is unused (corresponds
  // to outX=-1 spillover from the leftmost pixel) and index targetW+1 is the
  // rightmost overflow slot.  We index errCur[outX+1] / errNext[outX+1] so
  // that ±1 reaches without going negative.
  int16_t* errCur = ctx.errCur;
  int16_t* errNext = ctx.errNext;

  for (int outX = 0; outX < targetW; ++outX) {
    int g = static_cast<int>(ctx.targetGrayRow[outX]) + static_cast<int>(errCur[outX + 1]);
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    const int bit = (g >= 128) ? 1 : 0;
    if (bit) {
      // 1-bit BMP convention (matches writeBmpHeader1bit): 1 = white.
      outRow[outX >> 3] |= static_cast<uint8_t>(0x80 >> (outX & 7));
    }
    const int err = g - (bit ? 255 : 0);
    // 7/16 right, 3/16 down-left, 5/16 down, 1/16 down-right.
    errCur[outX + 2] = static_cast<int16_t>(errCur[outX + 2] + (err * 7) / 16);
    errNext[outX] = static_cast<int16_t>(errNext[outX] + (err * 3) / 16);
    errNext[outX + 1] = static_cast<int16_t>(errNext[outX + 1] + (err * 5) / 16);
    errNext[outX + 2] = static_cast<int16_t>(errNext[outX + 2] + (err * 1) / 16);
  }

  // Advance error rows: errNext becomes the new errCur; the old errCur is
  // zeroed and reused as the next errNext.
  std::swap(ctx.errCur, ctx.errNext);
  memset(ctx.errNext, 0, sizeof(int16_t) * (ctx.targetW + 2));
}

// Flush the dithered output row to disk.
bool writeOutputRow(DecodeCtx& ctx) {
  snapix::spi::SharedBusLock busLock;
  if (!busLock) {
    LOG_ERR(TAG, "Shared SPI lock unavailable for BMP row write");
    ctx.writeError = true;
    return false;
  }
  if (ctx.bmpOut->write(ctx.outRow, ctx.bytesPerRow) != static_cast<size_t>(ctx.bytesPerRow)) {
    LOG_ERR(TAG, "Failed to write BMP row");
    ctx.writeError = true;
    return false;
  }
  return true;
}

// Finalize the currently-accumulated target row: divide vertSum by count to
// get the post-vertical-downscale gray row, run horizontal area-average +
// Floyd-Steinberg, write to disk.  Resets vertSum / vertSumCount for the next
// target row.
bool finalizeTargetRow(DecodeCtx& ctx) {
  if (ctx.currentTargetY < 0 || ctx.vertSumCount <= 0) {
    return true;  // nothing to do (very first call)
  }

  // Stage a `decodedW`-wide gray row from the vertical accumulator.  We
  // reuse mcuBand's first row as scratch — at this point the MCU band that
  // generated this target row is no longer needed (its rows are all folded
  // into vertSum already).
  uint8_t* gray = ctx.mcuBand;
  const uint32_t cnt = static_cast<uint32_t>(ctx.vertSumCount);
  for (int x = 0; x < ctx.decodedW; ++x) {
    const uint32_t avg = ctx.vertSum[x] / cnt;
    gray[x] = static_cast<uint8_t>(avg > 255 ? 255 : avg);
  }

  // Horizontal downscale into 8-bit gray at target width.
  areaAverageHoriz(gray, ctx.decodedW, ctx.targetGrayRow, ctx.targetW, ctx.srcXStart);

  // Floyd-Steinberg into 1-bpp output row.
  floydSteinbergRow(ctx);

  if (!writeOutputRow(ctx)) return false;

  // Reset accumulator for the next target row.
  memset(ctx.vertSum, 0, sizeof(uint32_t) * ctx.decodedW);
  ctx.vertSumCount = 0;
  return true;
}

// Process every source row currently in mcuBand[0..mcuBandHeight-1], where
// row 0 corresponds to source-Y `mcuBandY`.  For each source row, determine
// its target-Y; if that differs from currentTargetY, finalize the previous
// target row first and advance.  Then accumulate the source row's pixels into
// vertSum.
bool processBand(DecodeCtx& ctx) {
  for (int row = 0; row < ctx.mcuBandHeight; ++row) {
    const int srcY = ctx.mcuBandY + row;
    if (srcY < 0 || srcY >= ctx.decodedH) continue;
    const int targetY = sourceYToTargetY(srcY, ctx.decodedH, ctx.targetH);
    if (targetY != ctx.currentTargetY) {
      if (!finalizeTargetRow(ctx)) return false;
      ctx.currentTargetY = targetY;
    }
    const uint8_t* srcRow = ctx.mcuBand + row * ctx.decodedW;
    for (int x = 0; x < ctx.decodedW; ++x) {
      ctx.vertSum[x] += srcRow[x];
    }
    ++ctx.vertSumCount;
  }
  return true;
}

// JPEGDEC draw callback — copy the 8-bit grayscale chunk into mcuBand at
// (chunkX, row-in-band).  Triggers band processing whenever a new chunkY
// arrives.  Note: in EIGHT_BIT_GRAYSCALE mode the draw callback may fire
// multiple times per MCU row (once per buffer-full of MCUs) when the image
// width exceeds JPEGDEC's internal MAX_BUFFERED_PIXELS / mcuCX threshold.
int jpegDraw(JPEGDRAW* pDraw) {
  auto* ctx = static_cast<DecodeCtx*>(pDraw->pUser);
  if (!ctx) return 1;

  if (ctx->shouldAbort && (*ctx->shouldAbort)()) {
    ctx->aborted = true;
    return 0;
  }
  if (ctx->writeError) return 0;

  const int chunkX = pDraw->x;
  const int chunkY = pDraw->y;
  const int chunkH = pDraw->iHeight;
  const int chunkW = pDraw->iWidthUsed;     // valid pixels (right-edge clip)
  const int packPitchPx = pDraw->iWidth;     // pack pitch (bytes-per-row in src)

  const auto* srcBytes = reinterpret_cast<const uint8_t*>(pDraw->pPixels);

  // If chunkY has advanced past the current band, process the previous band
  // before overwriting it.
  if (ctx->mcuBandY >= 0 && chunkY != ctx->mcuBandY) {
    if (!processBand(*ctx)) return 0;
    ctx->mcuBandHeight = 0;
  }
  if (ctx->mcuBandY < 0 || chunkY != ctx->mcuBandY) {
    ctx->mcuBandY = chunkY;
    ctx->mcuBandHeight = 0;
  }

  // The number of valid rows in this band is the max of (current chunkH,
  // existing mcuBandHeight); they should match across draws of the same band.
  if (chunkH > ctx->mcuBandHeight) ctx->mcuBandHeight = chunkH;

  // Copy each source row of the chunk into mcuBand at the right column offset.
  for (int row = 0; row < chunkH; ++row) {
    const int outY = chunkY + row;
    if (outY < 0 || outY >= ctx->decodedH) continue;

    const uint8_t* srcRow = srcBytes + row * packPitchPx;

    // Clip the chunk's right edge against the decoded width.
    int copyW = chunkW;
    if (chunkX + copyW > ctx->decodedW) copyW = ctx->decodedW - chunkX;
    if (copyW <= 0) continue;
    if (chunkX < 0) continue;  // not expected

    uint8_t* dstRow = ctx->mcuBand + row * ctx->decodedW + chunkX;
    memcpy(dstRow, srcRow, static_cast<size_t>(copyW));
  }

  ctx->yieldCounter++;
  if ((ctx->yieldCounter & 0x7) == 0) taskYIELD();

  return 1;
}

// Allocate all decode-context scratch buffers.  Returns true on success.  On
// failure, frees whatever was allocated.
bool allocateContext(DecodeCtx& ctx, int decodedW, int decodedH, int targetW, int targetH,
                     int mcuBandRows) {
  ctx.decodedW = decodedW;
  ctx.decodedH = decodedH;
  ctx.targetW = targetW;
  ctx.targetH = targetH;
  ctx.bytesPerRow = (targetW + 31) / 32 * 4;

  // MCU band: decodedW × mcuBandRows bytes.  Worst case ~534 × 16 = 8.5 KB.
  const size_t mcuBandSize = static_cast<size_t>(decodedW) * static_cast<size_t>(mcuBandRows);
  ctx.mcuBand = new (std::nothrow) uint8_t[mcuBandSize];
  if (!ctx.mcuBand) return false;
  memset(ctx.mcuBand, 0, mcuBandSize);

  ctx.vertSum = new (std::nothrow) uint32_t[decodedW];
  if (!ctx.vertSum) return false;
  memset(ctx.vertSum, 0, sizeof(uint32_t) * decodedW);
  ctx.vertSumCount = 0;
  ctx.currentTargetY = -1;
  ctx.mcuBandY = -1;
  ctx.mcuBandHeight = 0;

  ctx.targetGrayRow = new (std::nothrow) uint8_t[targetW];
  if (!ctx.targetGrayRow) return false;

  ctx.outRow = new (std::nothrow) uint8_t[ctx.bytesPerRow];
  if (!ctx.outRow) return false;
  memset(ctx.outRow, 0, ctx.bytesPerRow);

  // Floyd-Steinberg error rows.  Pad ±1 on each side so the spill-over math
  // stays in-bounds at the edges.
  ctx.errCur = new (std::nothrow) int16_t[targetW + 2];
  if (!ctx.errCur) return false;
  memset(ctx.errCur, 0, sizeof(int16_t) * (targetW + 2));

  ctx.errNext = new (std::nothrow) int16_t[targetW + 2];
  if (!ctx.errNext) return false;
  memset(ctx.errNext, 0, sizeof(int16_t) * (targetW + 2));

  // Precompute horizontal source-X column boundaries.
  ctx.srcXStart = new (std::nothrow) int[targetW + 1];
  if (!ctx.srcXStart) return false;
  for (int outX = 0; outX <= targetW; ++outX) {
    // 16.16 fixed-point: srcX = outX * decodedW * 65536 / targetW; floor by >>16.
    const int64_t v = (static_cast<int64_t>(outX) * static_cast<int64_t>(decodedW) * 65536LL) /
                      static_cast<int64_t>(targetW);
    ctx.srcXStart[outX] = static_cast<int>(v >> 16);
  }
  // Clamp boundaries.
  ctx.srcXStart[0] = 0;
  ctx.srcXStart[targetW] = decodedW;

  return true;
}

void freeContext(DecodeCtx& ctx) {
  delete[] ctx.mcuBand;
  delete[] ctx.vertSum;
  delete[] ctx.targetGrayRow;
  delete[] ctx.outRow;
  delete[] ctx.errCur;
  delete[] ctx.errNext;
  delete[] ctx.srcXStart;
  ctx.mcuBand = nullptr;
  ctx.vertSum = nullptr;
  ctx.targetGrayRow = nullptr;
  ctx.outRow = nullptr;
  ctx.errCur = nullptr;
  ctx.errNext = nullptr;
  ctx.srcXStart = nullptr;
}

// ---------------------------------------------------------------------------
// Common implementation for every public entry point.  All flavours share the
// open / scale / decode / dither pipeline; they only differ in target box
// dimensions.
// ---------------------------------------------------------------------------
bool decodeImpl(FsFile& jpegFile, Print& bmpOut, int maxW, int maxH,
                const std::function<bool()>& shouldAbort) {
  if (!jpegFile) return false;

  {
    snapix::spi::SharedBusLock lk;
    if (!lk || !jpegFile.seek(0)) return false;
  }

  const int32_t fileSize = static_cast<int32_t>(jpegFile.size());
  if (fileSize <= 0) return false;

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

  std::unique_ptr<JPEGDEC> jpeg(new (std::nothrow) JPEGDEC());
  if (!jpeg) {
    LOG_ERR(TAG, "JPEG OOM allocating JPEGDEC workspace");
    return false;
  }

  if (jpeg->open(&pump, fileSize, jpegClose, jpegRead, jpegSeek, jpegDraw) == 0) {
    LOG_ERR(TAG, "JPEGDEC open failed: err=%d", jpeg->getLastError());
    return false;
  }

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

  // Safety limits to prevent memory blowups on ESP32-C3.
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (srcW > MAX_IMAGE_WIDTH || srcH > MAX_IMAGE_HEIGHT) {
    LOG_ERR(TAG, "JPEG too large (%dx%d), max supported: %dx%d", srcW, srcH, MAX_IMAGE_WIDTH,
            MAX_IMAGE_HEIGHT);
    jpeg->close();
    return false;
  }

  // Pick the largest hardware scale that still leaves us ≥ the target box.
  // If the source already fits in the box, scale=0 (no hardware scale).
  int effMaxW = maxW > 0 ? maxW : srcW;
  int effMaxH = maxH > 0 ? maxH : srcH;
  const ScaleResult scale = chooseHardwareScale(srcW, srcH, effMaxW, effMaxH);
  const int decodedW = scale.decodedW;
  const int decodedH = scale.decodedH;
  if (decodedW <= 0 || decodedH <= 0) {
    LOG_ERR(TAG, "JPEG decoded dimensions degenerate %dx%d", decodedW, decodedH);
    jpeg->close();
    return false;
  }

  // Compute target dims: aspect-preserving, capped at maxW/maxH.  We mirror
  // FB2::scaledFit() to the byte (using src dims, not decoded dims) so the
  // BMP comes out at exactly the dimensions ImageBlock placed in its layout
  // — drawBitmap then renders 1:1 with no scaling.  Truncation matches
  // scaledFit's `(int)(srcW * scale)` cast.
  int targetW;
  int targetH;
  if (srcW <= effMaxW && srcH <= effMaxH) {
    targetW = srcW;
    targetH = srcH;
  } else {
    const double sx = static_cast<double>(effMaxW) / static_cast<double>(srcW);
    const double sy = static_cast<double>(effMaxH) / static_cast<double>(srcH);
    const double scaleD = sx < sy ? sx : sy;
    targetW = static_cast<int>(srcW * scaleD);
    targetH = static_cast<int>(srcH * scaleD);
  }
  if (targetW < 1) targetW = 1;
  if (targetH < 1) targetH = 1;
  // We only ever software-downscale: clamp target to the hardware-scaled
  // decoded dimensions so the area-average step never has to upscale.  In
  // practice this only fires when (decodedW < effMaxW), which means the
  // source already fit through hardware scaling alone.
  if (targetW > decodedW) targetW = decodedW;
  if (targetH > decodedH) targetH = decodedH;

  LOG_INF(TAG, "JPEG %dx%d -> decoded %dx%d -> target %dx%d (hwscale=%d)", srcW, srcH, decodedW,
          decodedH, targetW, targetH, scale.option);

  // Configure JPEGDEC for grayscale output.  Note: decode() rather than
  // decodeDither() — we run our own Floyd-Steinberg straight to 1-bit
  // because the BW e-paper pipeline already collapses 2-bit dithered values
  // to {black, white} anyway.
  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);

  // MCU height after hardware scale.  Subsample 0x22 (4:2:0) yields 16, all
  // others 8; halve again per scale shift.  We allocate for the worst case
  // (16) so a single buffer covers any subsample.
  const int mcuBandRows = 16;

  DecodeCtx ctx;
  ctx.bmpOut = &bmpOut;
  ctx.shouldAbort = (shouldAbort ? &shouldAbort : nullptr);
  if (!allocateContext(ctx, decodedW, decodedH, targetW, targetH, mcuBandRows)) {
    LOG_ERR(TAG, "JPEG OOM allocating decode scratch (decW=%d, tgtW=%d)", decodedW, targetW);
    freeContext(ctx);
    jpeg->close();
    return false;
  }

  // Write BMP header (under SharedBusLock so it doesn't interleave with
  // display SPI traffic).
  {
    snapix::spi::SharedBusLock lk;
    if (!lk) {
      LOG_ERR(TAG, "Shared SPI lock unavailable for BMP header write");
      freeContext(ctx);
      jpeg->close();
      return false;
    }
    writeBmpHeader1bit(bmpOut, targetW, targetH);
  }

  jpeg->setUserPointer(&ctx);

  // Decode.  With EIGHT_BIT_GRAYSCALE we get raw 8-bit Y values per pixel.
  const int decodeOptions = scale.option | JPEG_LUMA_ONLY;
  const int decodeOk = jpeg->decode(0, 0, decodeOptions);

  // Process the final MCU band that was buffered when decode() returned.
  if (!ctx.aborted && !ctx.writeError && ctx.mcuBandY >= 0 && ctx.mcuBandHeight > 0) {
    if (!processBand(ctx)) {
      // band processing flagged writeError; fall through to cleanup
    }
  }

  // Finalize the very last target row (its source rows have been folded into
  // vertSum but the row has not been written yet).
  if (!ctx.aborted && !ctx.writeError && ctx.currentTargetY >= 0) {
    finalizeTargetRow(ctx);
  }

  // Pad: if the target-row write count came up short (e.g. very last source
  // rows mapped to a target-Y that already finalized), emit zero-filled rows.
  // This is a safety-net for fractional-pixel rounding mismatches; the
  // computed targetH is the canonical row count in the BMP header.
  // (In practice with the rounding above we land exactly on targetH.)
  // We don't track total rows-written explicitly; the only way they'd come up
  // short is if the JPEGDEC decode bailed mid-image, in which case decodeOk
  // will be 0 and we report failure below.

  jpeg->close();

  const bool writeError = ctx.writeError;
  const bool aborted = ctx.aborted;
  freeContext(ctx);

  if (aborted) {
    LOG_INF(TAG, "JPEG decode aborted by caller");
    return false;
  }
  if (writeError) return false;
  if (decodeOk == 0) {
    LOG_ERR(TAG, "JPEG decode failed: err=%d", jpeg->getLastError());
    return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

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
                                                     int targetHeight, bool /*oneBit*/, bool /*quickMode*/,
                                                     const std::function<bool()>& shouldAbort) {
  // The "oneBit" / "quickMode" flags are silently ignored — every output is
  // now 1-bit Floyd-Steinberg-dithered to match the BW e-paper pipeline.
  return decodeImpl(jpegFile, bmpOut, targetWidth, targetHeight, shouldAbort);
}

// Default cover image target.  Same constants as the legacy converter so
// cache-invalidation on upgrade is the only behavioural break point.
namespace {
constexpr int kDefaultTargetW = 450;
constexpr int kDefaultTargetH = 750;
constexpr int kPreviewTargetSize = 64;
}  // namespace

bool JpegToBmpConverter::jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut) {
  return decodeImpl(jpegFile, bmpOut, kDefaultTargetW, kDefaultTargetH, nullptr);
}

bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight,
                                                     const std::function<bool()>& shouldAbort) {
  return decodeImpl(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, shouldAbort);
}

bool JpegToBmpConverter::jpegFileTo1BitBmpStream(FsFile& jpegFile, Print& bmpOut) {
  return decodeImpl(jpegFile, bmpOut, kDefaultTargetW, kDefaultTargetH, nullptr);
}

bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut,
                                                         int targetMaxWidth, int targetMaxHeight) {
  return decodeImpl(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, nullptr);
}

bool JpegToBmpConverter::jpegFileToBmpStreamQuick(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                  int targetMaxHeight,
                                                  const std::function<bool()>& shouldAbort) {
  return decodeImpl(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, shouldAbort);
}

// ---------------------------------------------------------------------------
// peekDimensions: returns SOURCE JPEG dims (not post-downscale dims).  The
// FB2 image-cache logic in `Fb2::cacheImage` calls scaledFit() on top of
// these to compute its own target box.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Tiny preview decode: 64×N 1-bit dithered.  Same downscale-and-dither
// pipeline as the full decode but with a tiny target box.  Output is ~64 ×
// proportional × 1bpp ≈ 0.5 KB and takes <1 s.
// ---------------------------------------------------------------------------
bool JpegToBmpConverter::jpegFileToBmpStreamPreview(FsFile& jpegFile, Print& bmpOut) {
  return decodeImpl(jpegFile, bmpOut, kPreviewTargetSize, kPreviewTargetSize, nullptr);
}
