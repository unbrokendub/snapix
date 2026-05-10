#pragma once

#include <FS.h>      // Arduino File (LittleFS)
#include <SdFat.h>   // FsFile (SD)

#include <cstdint>

#include "BitmapHelpers.h"

enum class BmpReaderError : uint8_t {
  Ok = 0,
  FileInvalid,
  SeekStartFailed,

  NotBMP,
  DIBTooSmall,

  BadPlanes,
  UnsupportedBpp,
  UnsupportedCompression,

  BadDimensions,
  ImageTooLarge,
  PaletteTooLarge,

  SeekPixelDataFailed,
  BufferTooSmall,
  OomRowBuffer,
  ShortReadRow,
};

class Bitmap {
 public:
  static const char* errorToString(BmpReaderError err);

  // SD/SDFat-backed file (cover thumbnails, EPUB pre-cached BMPs, etc.).  Reads
  // go through the SharedSpiLock, since the SD bus is shared with the e-paper
  // display.
  explicit Bitmap(FsFile& file, bool dithering = false) : sdFile_(&file), dithering(dithering) {}
  // Arduino LittleFS-backed file (FB2 inline-image BMPs since v2.0.53).  Reads
  // go straight to the internal-flash SPI bus — no SharedSpiLock needed, no
  // SD post-write recovery latency to amortise.  v2.0.70 streaming render
  // path uses this constructor so per-render heap pinning drops by ~12-15 KB.
  explicit Bitmap(File& file, bool dithering = false) : arduinoFile_(&file), dithering(dithering) {}
  ~Bitmap();
  BmpReaderError parseHeaders();
  BmpReaderError readRow(uint8_t* data, uint8_t* rowBuffer, int rowY) const;
  // v2.0.71: streaming raw read.  Identical to readRow's file-IO path but
  // returns the source bytes UNTOUCHED — no palette lookup, no dither, no
  // 2bpp packing.  drawBitmap uses this for the 1bpp/2bpp source formats
  // (which it can unpack inline) so the streaming render preserves the same
  // raw layout that the preloaded fast path delivered.  Without this method,
  // streaming a 1bpp BMP through readRow returned 2bpp packed bytes that
  // drawBitmap then mis-decoded as 1bpp — visible as ~2× horizontal stretch
  // and bottom-row truncation.
  BmpReaderError readRawRow(uint8_t* rowBuffer, int rowY) const;
  BmpReaderError rewindToData() const;
  int getWidth() const { return width; }
  int getHeight() const { return height; }
  bool isTopDown() const { return topDown; }
  bool hasGreyscale() const { return bpp > 1; }
  int getRowBytes() const { return rowBytes; }
  int getBpp() const { return bpp; }
  // Optimization hook for renderers: try to slurp every pixel row off SD in
  // ONE big SharedBusLock-protected read.  Without this, drawBitmap fires
  // ~hundreds of small file.read() calls — each one paying the SDFat
  // block-cache lookup + (post-write-storm) SD recovery latency, which
  // adds up to multi-second renders right after a JPEG decode flushes the
  // card cache.  Returns true if the slurp succeeded; subsequent
  // readRow() calls then memcpy from RAM instead of touching SD.
  bool preloadAllRows();
  bool isPreloaded() const { return preloadedRows_ != nullptr; }
  const uint8_t* preloadedRow(int rowIndex) const;

  // Single-shot file load: reads the WHOLE BMP file (header + palette +
  // pixel data) in ONE big file.read() under SharedBusLock, then parses
  // the header out of the RAM buffer.  Combines parseHeaders +
  // preloadAllRows into a single SD round-trip — the difference is huge
  // post-write SD-card recovery state where each individual SDFat op
  // pays a 100-300 ms latency cliff.  Subsequent preloadedRow()/readRow()
  // calls hit only RAM.  Returns Ok on success, or a parse-error code;
  // file is left untouched on failure (caller can fall back to the
  // streaming parseHeaders + preloadAllRows path).  Caller should NOT
  // also call parseHeaders.
  BmpReaderError parseAndLoadAll();

  // Borrowed-buffer mode: parse a BMP from an externally-owned byte buffer
  // WITHOUT touching the FsFile.  Caller retains ownership of `data` and
  // must keep it alive for the lifetime of this Bitmap.  Used by
  // ImageRenderCache hits — the cache holds the buffer, and this Bitmap
  // points into it for one render.  Skips both the SD allocation and
  // the file read entirely.
  BmpReaderError parseFromBorrowedBuffer(const uint8_t* data, size_t length);

 private:
  // Backing file: exactly one of sdFile_ / arduinoFile_ is non-null after
  // construction; parseFromBorrowedBuffer-only callers may construct via
  // FsFile and then never touch the file (the borrowed bytes are independent).
  FsFile* sdFile_ = nullptr;
  File* arduinoFile_ = nullptr;
  bool dithering = false;
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint32_t bfOffBits = 0;
  uint16_t bpp = 0;
  int rowBytes = 0;
  uint8_t paletteLum[256] = {};

  // Dithering state (mutable for const methods)
  mutable int prevRowY = -1;  // Track row progression for noise dithering
  // Tracks the BMP row that the file cursor currently points at.  Used by
  // readRow to detect non-sequential row requests (downscale srcY skips)
  // and seek to the correct offset before reading.  Reset by parseHeaders
  // and rewindToData.  Mutable because readRow is const.
  mutable int nextStreamRowY_ = 0;
  mutable AtkinsonDitherer* atkinsonDitherer = nullptr;
  mutable FloydSteinbergDitherer* fsDitherer = nullptr;

  // Optional whole-image preload buffer (heap, owned).  When set, readRow()
  // and the renderer's preloadedRow() helper bypass file.read entirely.
  // For preloadAllRows() this points at a buffer of just the pixel data;
  // for parseAndLoadAll() it points INTO a larger buffer that also
  // contains the BMP header — preloadedFileStart_ holds the alloc'd
  // pointer for the destructor to free.
  uint8_t* preloadedRows_ = nullptr;
  uint8_t* preloadedFileStart_ = nullptr;

  // True when preloadedRows_/preloadedFileStart_ point at memory we own
  // (default — destructor will free).  False when we've borrowed bytes
  // from an external owner (ImageRenderCache); destructor must NOT free.
  bool preloadedOwned_ = true;
};
