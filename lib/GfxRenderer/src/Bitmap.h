#pragma once

#include <SdFat.h>

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

  explicit Bitmap(FsFile& file, bool dithering = false) : file(file), dithering(dithering) {}
  ~Bitmap();
  BmpReaderError parseHeaders();
  BmpReaderError readRow(uint8_t* data, uint8_t* rowBuffer, int rowY) const;
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

 private:
  static uint16_t readLE16(FsFile& f);
  static uint32_t readLE32(FsFile& f);

  FsFile& file;
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
  mutable AtkinsonDitherer* atkinsonDitherer = nullptr;
  mutable FloydSteinbergDitherer* fsDitherer = nullptr;

  // Optional whole-image preload buffer (heap, owned).  When set, readRow()
  // and the renderer's preloadedRow() helper bypass file.read entirely.
  uint8_t* preloadedRows_ = nullptr;
};
