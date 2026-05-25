#include "JpegToBmpConverter.h"

#include <Arduino.h>
#include <FS.h>          // v2.0.79: fs::File for LittleFS-backed JPEG inputs
#include <JPEGDEC.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#define TAG "JPEG"
#include <SdFat.h>
#include <SharedSpiLock.h>

#include <algorithm>
#include <cmath>  // v2.0.68: std::sqrt for proportional target downscale
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

// ---------------------------------------------------------------------------
// v2.0.79: LittleFS-backed pump.  EPUB chapter image temp files (and EPUB
// cover/thumb temp files) live on LittleFS post-v2.0.73; the existing FsFile
// pump only knows how to read from SD via SDFat.  We mirror the pump struct
// + read/seek callbacks with an `fs::File` underneath so JPEGDEC can pull
// bytes through the same hot path.
//
// Notes:
//   * fs::File::read returns size_t (not int — no error sentinel; 0 means
//     EOF).  We treat 0 as "no more data" and break the fill loop.
//   * LittleFS lives on a different SPI controller than the SD card / display
//     bus, so reads don't need the SharedBusLock.  Acquiring it here would
//     pointlessly stall the display thread.
//   * Buffer size matches the FsFile pump (4 KB) for consistent JPEGDEC
//     read-ahead behaviour.
// ---------------------------------------------------------------------------
struct LittleFsJpegPumpCtx {
  fs::File* file;
  static constexpr size_t kBufferSize = 4096;
  uint8_t* buffer;
  size_t bufferPos;
  size_t bufferFilled;
  int32_t streamPos;
};

int32_t jpegReadLittleFs(JPEGFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  auto* ctx = static_cast<LittleFsJpegPumpCtx*>(pFile->fHandle);
  if (!ctx || !ctx->file || !*ctx->file) return 0;

  int32_t copied = 0;
  while (copied < iLen) {
    if (ctx->bufferPos >= ctx->bufferFilled) {
      const size_t readResult = ctx->file->read(ctx->buffer, LittleFsJpegPumpCtx::kBufferSize);
      ctx->bufferPos = 0;
      if (readResult == 0) {
        ctx->bufferFilled = 0;
        break;
      }
      ctx->bufferFilled = readResult;
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

int32_t jpegSeekLittleFs(JPEGFILE* pFile, int32_t iPosition) {
  auto* ctx = static_cast<LittleFsJpegPumpCtx*>(pFile->fHandle);
  if (!ctx || !ctx->file || !*ctx->file) return 0;

  if (iPosition == ctx->streamPos) return 1;  // no-op; pump buffer still valid

  if (!ctx->file->seek(static_cast<uint32_t>(iPosition))) return 0;
  ctx->bufferPos = 0;
  ctx->bufferFilled = 0;
  ctx->streamPos = iPosition;
  return 1;
}

// Forward declaration — defined ~600 lines below in this same anonymous
// namespace, alongside decodeImpl(FsFile&,...).
bool decodeImplCallbacks(JPEG_READ_CALLBACK pfnRead, JPEG_SEEK_CALLBACK pfnSeek,
                         JPEG_CLOSE_CALLBACK pfnClose, void* fHandle, int32_t iSize,
                         Print& bmpOut, int maxW, int maxH,
                         const std::function<bool()>& shouldAbort);

// LittleFS pump adapter: sets up a LittleFsJpegPumpCtx around the file and
// forwards to decodeImplCallbacks.  Mirror of decodeImpl(FsFile&,...).
bool decodeImplLittleFs(fs::File& jpegFile, Print& bmpOut, int maxW, int maxH,
                        const std::function<bool()>& shouldAbort) {
  if (!jpegFile) return false;
  if (!jpegFile.seek(0)) return false;

  const int32_t fileSize = static_cast<int32_t>(jpegFile.size());
  if (fileSize <= 0) return false;

  std::unique_ptr<uint8_t[]> pumpBuf(new (std::nothrow) uint8_t[LittleFsJpegPumpCtx::kBufferSize]);
  if (!pumpBuf) {
    LOG_ERR(TAG, "JPEG OOM allocating %u-byte LittleFS pump buffer",
            static_cast<unsigned>(LittleFsJpegPumpCtx::kBufferSize));
    return false;
  }

  LittleFsJpegPumpCtx pump = {.file = &jpegFile,
                              .buffer = pumpBuf.get(),
                              .bufferPos = 0,
                              .bufferFilled = 0,
                              .streamPos = 0};

  return decodeImplCallbacks(jpegReadLittleFs, jpegSeekLittleFs, jpegClose, &pump, fileSize,
                             bmpOut, maxW, maxH, shouldAbort);
}

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

// ---------------------------------------------------------------------------
// Persistent decode scratch arena.
//
// v2.0.56 background: each decode used to allocate 7 separate heap blocks
// (mcuBand ~6 KB, vertSum 1.5 KB, srcXStart 1.5 KB, errCur/errNext 1.5 KB
// each, plus tiny outRow/targetGrayRow).  On a 320 KB-RAM ESP32-C3 with the
// persistent ~25 KB JPEGDEC instance already pinning a hole in the heap,
// 7 separate allocations carved up whatever was left into ever-smaller
// fragments.  Once the largest contiguous block dropped below the 8 KB
// abort-watchdog floor (or `mcuBandSize` itself ~6 KB couldn't allocate
// contiguously) decodes started failing and never recovered.
//
// v2.0.57 fix: ONE contiguous arena big enough for every scratch buffer,
// allocated lazily on first decode and grown only when a wider image needs
// more room.  All buffers carve out of this single block so the arena is
// the only long-lived heap chunk added by JPEG decode (in addition to the
// shared JPEGDEC).  Memory cost: ~13 KB pinned for a 379-wide source,
// ~20 KB for a 600-wide source.  In exchange, fragmentation per decode
// goes from 7 holes → 0 (the arena lives at a stable address forever).
// ---------------------------------------------------------------------------
struct DecodeArena {
  uint8_t* base = nullptr;
  size_t capacity = 0;
};

DecodeArena& getSharedArena() {
  static DecodeArena arena;
  return arena;
}

// Round up to 8 bytes for alignment of int32_t / uint32_t / int16_t carve-outs.
constexpr size_t kAlign = 8;
inline size_t roundUpAlign(size_t s) { return (s + kAlign - 1) & ~(kAlign - 1); }

// Compute total arena bytes needed for a given decoded/target geometry.
size_t computeArenaBytes(int decodedW, int targetW, int mcuBandRows) {
  size_t total = 0;
  total += roundUpAlign(static_cast<size_t>(decodedW) * static_cast<size_t>(mcuBandRows));  // mcuBand
  total += roundUpAlign(static_cast<size_t>(decodedW) * sizeof(uint32_t));                  // vertSum
  total += roundUpAlign(static_cast<size_t>(targetW));                                      // targetGrayRow
  total += roundUpAlign(static_cast<size_t>((targetW + 31) / 32 * 4));                      // outRow
  total += roundUpAlign(static_cast<size_t>(targetW + 2) * sizeof(int16_t));                // errCur
  total += roundUpAlign(static_cast<size_t>(targetW + 2) * sizeof(int16_t));                // errNext
  total += roundUpAlign(static_cast<size_t>(targetW + 1) * sizeof(int));                    // srcXStart
  return total;
}

// v2.0.59: predict the size of the resulting BMP file (1bpp BMP header +
// pixel data).  The arena gets sized to also hold this so ImageBlock::render
// can later slurp the BMP back into the SAME memory without paying a fresh
// 26-60 KB heap allocation that routinely OOMs on a fragmented heap.
size_t expectedBmpFileSize(int targetW, int targetH) {
  // BMP1bpp layout: 14 (file header) + 40 (DIB header) + 8 (2-entry palette) +
  // rowStride * targetH where rowStride is targetW bits padded to multiples
  // of 4 bytes.  Matches writeBmpHeader1bit + processBand row write.
  const size_t headerBytes = 62;
  const size_t rowStride = static_cast<size_t>((targetW + 31) / 32) * 4;
  return headerBytes + rowStride * static_cast<size_t>(targetH);
}

// Allocate all decode-context scratch buffers from the persistent arena.
// Returns true on success.  Failure means even a single contiguous arena of
// the required size cannot be allocated — caller should bail and let the
// async-jobs retry counter mark the binary as `.failed` after kMaxRetries.
bool allocateContext(DecodeCtx& ctx, int decodedW, int decodedH, int targetW, int targetH,
                     int mcuBandRows) {
  ctx.decodedW = decodedW;
  ctx.decodedH = decodedH;
  ctx.targetW = targetW;
  ctx.targetH = targetH;
  ctx.bytesPerRow = (targetW + 31) / 32 * 4;

  // v2.0.70 arena scope: scratch buffers ONLY.  Pre-v2.0.70 we also sized
  // for the resulting BMP file (so ImageBlock::render could slurp the BMP
  // back into the same memory) — that pinned 24-36 KB permanently and was
  // the root cause of the v2.0.67-68 fragmentation lockups.  ImageBlock now
  // streams BMPs straight off LittleFS row-by-row via the v2.0.70 streaming
  // Bitmap constructor, so the arena no longer needs to hold them.  Net win:
  // arena drops from ~24 KB to ~13 KB pinned.
  const size_t scratchBytes = computeArenaBytes(decodedW, targetW, mcuBandRows);

  DecodeArena& arena = getSharedArena();

  // Try to grow to scratch size if the existing arena is smaller.  Allocate
  // the bigger buffer BEFORE freeing the old one — otherwise an interleaving
  // alloc could grab the hole and we'd lose both the old arena AND fail the
  // bigger alloc.  (At this point we briefly need ~old + new bytes
  // simultaneously — fine on a 320 KB-RAM ESP32-C3.)
  if (arena.capacity < scratchBytes) {
    uint8_t* newBase = new (std::nothrow) uint8_t[scratchBytes];
    if (!newBase) {
      LOG_ERR(TAG,
              "JPEG decode arena grow failed (cap=%u, scratch=%u, free=%u largest=%u)",
              static_cast<unsigned>(arena.capacity), static_cast<unsigned>(scratchBytes),
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
      return false;
    }
    delete[] arena.base;
    arena.base = newBase;
    arena.capacity = scratchBytes;
    LOG_INF(TAG, "JPEG decode arena (re)allocated: %u bytes (decW=%d tgtW=%d)",
            static_cast<unsigned>(scratchBytes), decodedW, targetW);
  }

  // Carve buffers out of the arena.  Order matches computeArenaBytes() above.
  uint8_t* p = arena.base;
  ctx.mcuBand = p;
  p += roundUpAlign(static_cast<size_t>(decodedW) * static_cast<size_t>(mcuBandRows));

  ctx.vertSum = reinterpret_cast<uint32_t*>(p);
  p += roundUpAlign(static_cast<size_t>(decodedW) * sizeof(uint32_t));

  ctx.targetGrayRow = p;
  p += roundUpAlign(static_cast<size_t>(targetW));

  ctx.outRow = p;
  p += roundUpAlign(static_cast<size_t>(ctx.bytesPerRow));

  ctx.errCur = reinterpret_cast<int16_t*>(p);
  p += roundUpAlign(static_cast<size_t>(targetW + 2) * sizeof(int16_t));

  ctx.errNext = reinterpret_cast<int16_t*>(p);
  p += roundUpAlign(static_cast<size_t>(targetW + 2) * sizeof(int16_t));

  ctx.srcXStart = reinterpret_cast<int*>(p);
  p += roundUpAlign(static_cast<size_t>(targetW + 1) * sizeof(int));

  // Per-decode initialization.  The arena is reused, so previous-decode
  // residue must be cleared from buffers the algorithm reads before writing.
  const size_t mcuBandSize = static_cast<size_t>(decodedW) * static_cast<size_t>(mcuBandRows);
  memset(ctx.mcuBand, 0, mcuBandSize);
  memset(ctx.vertSum, 0, sizeof(uint32_t) * decodedW);
  memset(ctx.outRow, 0, ctx.bytesPerRow);
  memset(ctx.errCur, 0, sizeof(int16_t) * (targetW + 2));
  memset(ctx.errNext, 0, sizeof(int16_t) * (targetW + 2));

  // Precompute horizontal source-X column boundaries.
  for (int outX = 0; outX <= targetW; ++outX) {
    // 16.16 fixed-point: srcX = outX * decodedW * 65536 / targetW; floor by >>16.
    const int64_t v = (static_cast<int64_t>(outX) * static_cast<int64_t>(decodedW) * 65536LL) /
                      static_cast<int64_t>(targetW);
    ctx.srcXStart[outX] = static_cast<int>(v >> 16);
  }
  // Clamp boundaries.
  ctx.srcXStart[0] = 0;
  ctx.srcXStart[targetW] = decodedW;

  ctx.vertSumCount = 0;
  ctx.currentTargetY = -1;
  ctx.mcuBandY = -1;
  ctx.mcuBandHeight = 0;

  return true;
}

// Arena-backed: scratch pointers all alias into the persistent arena, so we
// only clear the DecodeCtx fields (no `delete[]`).  The arena itself stays
// pinned for the rest of the session — see DecodeArena comment above.
void freeContext(DecodeCtx& ctx) {
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
// Persistent JPEGDEC instance — allocated on first decode and reused for the
// rest of the session.  `sizeof(JPEGDEC)` is ~25 KB on ESP32-C3 (the bulk is
// internal Huffman tables, MCU buffer, and dither workspace embedded in the
// struct).  v2.0.x re-allocated this on every decode, which churned the heap
// hard enough that fragmentation eventually dropped the largest free block
// below the BG worker's `isHeapCritical` threshold (10 KB) and started
// aborting JPEG decodes mid-flight.  v2.0.54 holds onto the instance so the
// 25 KB chunk lives at a stable address — heap fluctuations during decode
// are now bounded by just the scratch buffers (~5-10 KB).
// v2.0.81: lifted from function-local static to anonymous-namespace so the
// public release entry point can free it on demand.
JPEGDEC* gSharedJpegDec = nullptr;

JPEGDEC* getSharedJpegDec() {
  if (!gSharedJpegDec) {
    gSharedJpegDec = new (std::nothrow) JPEGDEC();
    if (!gSharedJpegDec) {
      LOG_ERR(TAG, "JPEGDEC: OOM allocating shared workspace (~25 KB)");
    }
  }
  return gSharedJpegDec;
}

// Generic stream-source decoder, callable with any JPEGDEC pump (FsFile-backed
// JpegPumpCtx OR Base64JpegPump).  Caller passes the raw JPEGDEC callback ABI
// — pfnRead/pfnSeek/pfnClose + a void* handle that those callbacks understand.
// `iSize` is the upper-bound JPEG byte length (used by JPEGDEC for seek
// validation; small overshoot is harmless).  Used by the file-based wrapper
// `decodeImpl` AND the public stream entry point `jpegStreamToBmp`.
bool decodeImplCallbacks(JPEG_READ_CALLBACK pfnRead, JPEG_SEEK_CALLBACK pfnSeek,
                         JPEG_CLOSE_CALLBACK pfnClose, void* fHandle, int32_t iSize,
                         Print& bmpOut, int maxW, int maxH,
                         const std::function<bool()>& shouldAbort) {
  if (iSize <= 0) return false;

  JPEGDEC* jpeg = getSharedJpegDec();
  if (!jpeg) return false;  // OOM logged in getSharedJpegDec

  if (jpeg->open(fHandle, iSize, pfnClose, pfnRead, pfnSeek, jpegDraw) == 0) {
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

  // Compute the actual (aspect-preserving) target dims FIRST.  These — not
  // the maxW × maxH bounding box — are what the HW scale chooser needs to
  // honour: a 653×466 source aspect-fit into a 226×349 box produces 226×161
  // (width-bound), so HW /2 (decoded 326×233) is sufficient even though
  // 233 < 349.  Choosing HW scale against the box would wrongly force /1
  // and turn a ~3 s decode into a ~12 s decode.
  int effMaxW = maxW > 0 ? maxW : srcW;
  int effMaxH = maxH > 0 ? maxH : srcH;
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

  // v2.0.68: cap target so the resulting 1bpp BMP doesn't exceed an arena
  // budget the heap can actually carry alongside other long-lived pinned
  // allocations (JPEGDEC ~25 KB + resident pages ~30 KB + glyph cache + ...).
  //
  // Background: v2.0.59 sized the arena to max(decode_scratch, BMP_size) so
  // the render-side BMP slurp could re-use the same memory.  For small
  // images that's a single ~12-26 KB pinned chunk, fine.  For large images
  // (e.g. 653×884 source → 452×611 target → 36 KB BMP) the arena alone
  // grabs the largest contiguous block, leaving the runtime with `largest=
  // 7668` permanently — every subsequent decode aborts at the heap-critical
  // check, BG cache stops, and the user sees placeholders forever.
  //
  // The cap is a quality/visibility trade-off: shrink the displayed image
  // so the system stays responsive.  24 KB BMP ≈ 350×450 pixels at 1bpp,
  // which still occupies a meaningful fraction of an X4 portrait page and
  // is plenty for inline FB2 illustrations.  Sources that already fit
  // under the cap pass through untouched.
  constexpr size_t kMaxBmpBytes = 24 * 1024;
  {
    const size_t projectedBmp = expectedBmpFileSize(targetW, targetH);
    if (projectedBmp > kMaxBmpBytes) {
      const double shrink = std::sqrt(static_cast<double>(kMaxBmpBytes) /
                                     static_cast<double>(projectedBmp));
      const int newW = std::max(1, static_cast<int>(targetW * shrink));
      const int newH = std::max(1, static_cast<int>(targetH * shrink));
      LOG_INF(TAG,
              "JPEG target capped %dx%d → %dx%d (projected BMP %u B exceeds %u B arena budget)",
              targetW, targetH, newW, newH, static_cast<unsigned>(projectedBmp),
              static_cast<unsigned>(kMaxBmpBytes));
      targetW = newW;
      targetH = newH;
    }
  }

  // Pick the largest hardware scale that still leaves decoded ≥ actual
  // target.  This is the only valid lower bound — the software downscale
  // step that follows can ONLY downscale, so decoded must exceed target.
  const ScaleResult scale = chooseHardwareScale(srcW, srcH, targetW, targetH);
  const int decodedW = scale.decodedW;
  const int decodedH = scale.decodedH;
  if (decodedW <= 0 || decodedH <= 0) {
    LOG_ERR(TAG, "JPEG decoded dimensions degenerate %dx%d", decodedW, decodedH);
    jpeg->close();
    return false;
  }

  // Defence-in-depth: if rounding pushed targetW/H above decodedW/H (can
  // happen at the lowest HW scale on small sources), clamp.  The
  // area-average step downscales only.
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

// FsFile-backed adapter: sets up a JpegPumpCtx around the file and forwards
// to decodeImplCallbacks.  Keeps the existing file-based public entry points
// working unchanged.
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

  return decodeImplCallbacks(jpegRead, jpegSeek, jpegClose, &pump, fileSize, bmpOut, maxW, maxH,
                             shouldAbort);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

// v2.0.59: lend the persistent decode arena to non-decoder callers (e.g.
// ImageBlock::render slurping a BMP file from LittleFS).  Now that v2.0.70
// streams BMPs row-by-row in the renderer, this is dead in-tree but kept
// public for any out-of-tree caller that still wants opportunistic borrow.
// Will return nullptr for typical BMP-sized requests since the arena is no
// longer sized to fit them.
uint8_t* JpegToBmpConverter::borrowDecodeArena(size_t needed) {
  DecodeArena& arena = getSharedArena();
  if (arena.base == nullptr || arena.capacity < needed) return nullptr;
  return arena.base;
}

size_t JpegToBmpConverter::decodeArenaCapacity() {
  return getSharedArena().capacity;
}

// v2.0.63: pre-allocate JPEGDEC + arena from setup() to keep them at one end
// of the heap.  See header comment for fragmentation rationale.
void JpegToBmpConverter::warmup(size_t arenaBytes) {
  // Force JPEGDEC instance (~25 KB).  getSharedJpegDec() lazily allocates
  // on first call; subsequent calls return the same pointer.
  JPEGDEC* jpeg = getSharedJpegDec();
  if (!jpeg) {
    LOG_ERR(TAG, "JPEG warmup: JPEGDEC instance allocation failed");
    return;
  }
  LOG_INF(TAG, "JPEG warmup: JPEGDEC instance ready (free=%u largest=%u)",
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  if (arenaBytes == 0) return;

  // Force arena to the requested initial size.  Subsequent decodes may grow
  // it if a wider image needs more, but typical book images fit in 32 KB.
  DecodeArena& arena = getSharedArena();
  if (arena.capacity >= arenaBytes) {
    return;  // Already at least this size — nothing to do.
  }

  uint8_t* newBase = new (std::nothrow) uint8_t[arenaBytes];
  if (!newBase) {
    LOG_ERR(TAG, "JPEG warmup: arena %u-byte alloc failed (free=%u largest=%u)",
            static_cast<unsigned>(arenaBytes),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return;
  }
  delete[] arena.base;
  arena.base = newBase;
  arena.capacity = arenaBytes;
  LOG_INF(TAG, "JPEG warmup: arena %u bytes ready (free=%u largest=%u)",
          static_cast<unsigned>(arenaBytes),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

// v2.0.81: heap-defrag release entry points.  See header for rationale and
// safety constraints.  These work in tandem: releasing only the arena
// (~7-20 KB) leaves JPEGDEC's pin (~25 KB), which still bisects the heap
// into <23.5 KB chunks; releasing only JPEGDEC leaves the arena pin.  The
// combined release returns one ~40 KB contiguous run.
void JpegToBmpConverter::releaseDecodeArena() {
  DecodeArena& arena = getSharedArena();
  if (!arena.base) return;
  const size_t freed = arena.capacity;
  delete[] arena.base;
  arena.base = nullptr;
  arena.capacity = 0;
  LOG_INF(TAG, "JPEG arena released (%u bytes freed; free=%u largest=%u)",
          static_cast<unsigned>(freed),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

void JpegToBmpConverter::releaseSharedInstance() {
  if (!gSharedJpegDec) return;
  delete gSharedJpegDec;
  gSharedJpegDec = nullptr;
  LOG_INF(TAG, "JPEGDEC instance released (~25 KB; free=%u largest=%u)",
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

void JpegToBmpConverter::releaseAllPersistent() {
  releaseDecodeArena();
  releaseSharedInstance();
}

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

// Generic stream-source entry point — caller supplies JPEGDEC's own callback
// ABI plus an opaque handle.  Backbone of v2.0.50's "no pending JPEG file"
// pipeline: Fb2::decodeImageDirect feeds a Base64JpegPump in here, JPEGDEC
// pulls bytes through the pump (which decodes base64 from FB2 source
// inline), and the BMP is written straight to the destination.  No
// intermediate JPEG file ever lands on SD.
bool JpegToBmpConverter::jpegStreamToBmp(JPEG_READ_CALLBACK pfnRead, JPEG_SEEK_CALLBACK pfnSeek,
                                          JPEG_CLOSE_CALLBACK pfnClose, void* fHandle,
                                          int32_t fileSize, Print& bmpOut, int targetMaxWidth,
                                          int targetMaxHeight,
                                          const std::function<bool()>& shouldAbort) {
  return decodeImplCallbacks(pfnRead, pfnSeek, pfnClose, fHandle, fileSize, bmpOut, targetMaxWidth,
                             targetMaxHeight, shouldAbort);
}

// v2.0.79: LittleFS-backed entry point.  Used by ImageConverterFactory when
// the input image temp file lives on internal flash (EPUB chapter inline
// images and EPUB cover/thumb temp files since v2.0.73).  Output Print&
// may still be an SD FsFile, a LittleFS fs::File, or any other Stream — the
// converter doesn't care which.
bool JpegToBmpConverter::jpegLittleFsFileToBmpStreamWithSize(fs::File& jpegFile, Print& bmpOut,
                                                              int targetMaxWidth, int targetMaxHeight,
                                                              const std::function<bool()>& shouldAbort) {
  return decodeImplLittleFs(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, shouldAbort);
}

// v2.0.83: header-only peek for LittleFS-backed JPEGs.  Parallels the
// existing peekDimensions(FsFile&,...) but takes an Arduino fs::File.  Used
// by ChapterHtmlSlimParser::cacheImage when running in async-decode mode:
// it extracts the JPEG to a LittleFS temp file, peeks dimensions for the
// page-layout pass, and defers the full BMP conversion to a BG worker.
//
// JPEGDEC open() parses the SOF marker as part of its lazy init, which on
// a 240×300 source touches ~1-3 KB of file data.  Even on a Calibre EPUB's
// deflate-everything packing the temp JPEG is already inflated by the
// extract step, so peek runs in ~30-50 ms.
bool JpegToBmpConverter::peekDimensionsLittleFs(fs::File& jpegFile, int& outWidth, int& outHeight) {
  outWidth = 0;
  outHeight = 0;
  if (!jpegFile) return false;
  if (!jpegFile.seek(0)) return false;

  const int32_t fileSize = static_cast<int32_t>(jpegFile.size());
  if (fileSize <= 0) return false;

  std::unique_ptr<uint8_t[]> pumpBuf(new (std::nothrow) uint8_t[LittleFsJpegPumpCtx::kBufferSize]);
  if (!pumpBuf) return false;

  LittleFsJpegPumpCtx pump = {.file = &jpegFile,
                              .buffer = pumpBuf.get(),
                              .bufferPos = 0,
                              .bufferFilled = 0,
                              .streamPos = 0};

  // Borrow the persistent JPEGDEC instance.  Open + read SOF + close —
  // does not allocate the decode arena (that only fires inside decode()).
  JPEGDEC* jpeg = getSharedJpegDec();
  if (!jpeg) return false;

  if (jpeg->open(&pump, fileSize, jpegClose, jpegReadLittleFs, jpegSeekLittleFs, jpegDraw) == 0) {
    LOG_DBG(TAG, "peekDimensionsLittleFs: open failed err=%d", jpeg->getLastError());
    return false;
  }
  if (jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE) {
    LOG_DBG(TAG, "peekDimensionsLittleFs: progressive JPEG not supported");
    jpeg->close();
    return false;
  }
  outWidth = jpeg->getWidth();
  outHeight = jpeg->getHeight();
  jpeg->close();
  return outWidth > 0 && outHeight > 0;
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
bool JpegToBmpConverter::jpegFileToBmpStreamPreview(FsFile& jpegFile, Print& bmpOut,
                                                    const std::function<bool()>& shouldAbort) {
  return decodeImpl(jpegFile, bmpOut, kPreviewTargetSize, kPreviewTargetSize, shouldAbort);
}

// ---------------------------------------------------------------------------
// Progressive-stage decoder: scales the (target_w × target_h) box by
// numerator/denominator and runs the standard decodeImpl pipeline.  This is
// the workhorse for the FB2 BG worker's 5-stage preview chain.
// ---------------------------------------------------------------------------
bool JpegToBmpConverter::jpegFileToBmpStreamFraction(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight, int numerator, int denominator,
                                                     const std::function<bool()>& shouldAbort) {
  if (denominator <= 0) denominator = 1;
  if (numerator <= 0) numerator = 1;
  const int scaledW = targetMaxWidth > 0 ? std::max(1, targetMaxWidth * numerator / denominator) : 0;
  const int scaledH = targetMaxHeight > 0 ? std::max(1, targetMaxHeight * numerator / denominator) : 0;
  return decodeImpl(jpegFile, bmpOut, scaledW, scaledH, shouldAbort);
}

bool JpegToBmpConverter::jpegFileToBmpStreamLow(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                int targetMaxHeight,
                                                const std::function<bool()>& shouldAbort) {
  return jpegFileToBmpStreamFraction(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, 1, 4, shouldAbort);
}

bool JpegToBmpConverter::jpegFileToBmpStreamMid(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                int targetMaxHeight,
                                                const std::function<bool()>& shouldAbort) {
  return jpegFileToBmpStreamFraction(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, 1, 2, shouldAbort);
}

bool JpegToBmpConverter::jpegFileToBmpStreamHigh(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                 int targetMaxHeight,
                                                 const std::function<bool()>& shouldAbort) {
  return jpegFileToBmpStreamFraction(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, 3, 4, shouldAbort);
}
