#include "Bitmap.h"

#include <SharedSpiLock.h>

#include <cstdlib>
#include <cstring>
#include <new>

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
// Note: For cover images, dithering is done in JpegToBmpConverter.cpp
// This file handles BMP reading - use simple quantization to avoid double-dithering
constexpr bool USE_ATKINSON = true;  // Use Atkinson dithering instead of Floyd-Steinberg
// ============================================================================

Bitmap::~Bitmap() {
  delete atkinsonDitherer;
  delete fsDitherer;
  // Free whichever pointer actually owns the heap allocation.  After
  // parseAndLoadAll(), preloadedFileStart_ is the alloc'd block and
  // preloadedRows_ is offset INTO it.  After preloadAllRows(),
  // preloadedFileStart_ stays nullptr and preloadedRows_ owns its own
  // buffer.
  if (preloadedFileStart_) {
    delete[] preloadedFileStart_;
  } else if (preloadedRows_) {
    delete[] preloadedRows_;
  }
  preloadedRows_ = nullptr;
  preloadedFileStart_ = nullptr;
}

BmpReaderError Bitmap::parseAndLoadAll() {
  if (!file) return BmpReaderError::FileInvalid;
  // Defensive: free any prior allocation so a re-parse starts clean.
  if (preloadedFileStart_) {
    delete[] preloadedFileStart_;
    preloadedFileStart_ = nullptr;
    preloadedRows_ = nullptr;
  } else if (preloadedRows_) {
    delete[] preloadedRows_;
    preloadedRows_ = nullptr;
  }

  uint32_t fileSize;
  {
    snapix::spi::SharedBusLock lk;
    if (!lk || !file.seek(0)) return BmpReaderError::SeekStartFailed;
    fileSize = file.size();
  }
  if (fileSize < 62) return BmpReaderError::FileInvalid;

  // Cap at 256 KB — anything bigger and we'd rather stream.  In practice
  // FB2 inline image BMPs after v2.0.38's 1-bit-at-target redesign top
  // out around 30 KB; cover thumbs are similar.
  constexpr uint32_t kMaxLoadAll = 256 * 1024;
  if (fileSize > kMaxLoadAll) return BmpReaderError::ImageTooLarge;

  preloadedRows_ = new (std::nothrow) uint8_t[fileSize];
  if (!preloadedRows_) return BmpReaderError::OomRowBuffer;

  // ONE big SharedBusLock-protected read of the entire file.  Pays
  // SDFat's 100-300 ms post-write-recovery latency exactly once instead
  // of 6+ times across parseHeaders + preloadAllRows + readRow paths.
  {
    snapix::spi::SharedBusLock lk;
    if (!lk || !file.seek(0)) {
      delete[] preloadedRows_;
      preloadedRows_ = nullptr;
      return BmpReaderError::FileInvalid;
    }
    if (file.read(preloadedRows_, fileSize) != static_cast<int>(fileSize)) {
      delete[] preloadedRows_;
      preloadedRows_ = nullptr;
      return BmpReaderError::FileInvalid;
    }
  }

  // Parse the header out of preloadedRows_ — same logic as parseHeaders
  // but reading from RAM instead of via file.read().
  const uint8_t* hdr = preloadedRows_;

  auto leU16 = [](const uint8_t* p) -> uint16_t {
    return static_cast<uint16_t>(p[0] | (uint16_t(p[1]) << 8));
  };
  auto leU32 = [](const uint8_t* p) -> uint32_t {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  };
  auto leI32 = [&leU32](const uint8_t* p) -> int32_t { return static_cast<int32_t>(leU32(p)); };

  if (leU16(hdr + 0) != 0x4D42) {
    delete[] preloadedRows_;
    preloadedRows_ = nullptr;
    return BmpReaderError::NotBMP;
  }
  bfOffBits = leU32(hdr + 10);
  const uint32_t biSize = leU32(hdr + 14);
  if (biSize < 40) {
    delete[] preloadedRows_;
    preloadedRows_ = nullptr;
    return BmpReaderError::DIBTooSmall;
  }
  width = leI32(hdr + 18);
  const int32_t rawHeight = leI32(hdr + 22);
  topDown = rawHeight < 0;
  height = topDown ? -rawHeight : rawHeight;
  const uint16_t planes = leU16(hdr + 26);
  bpp = leU16(hdr + 28);
  const uint32_t comp = leU32(hdr + 30);
  const uint32_t colorsUsed = leU32(hdr + 46);
  const bool validBpp = bpp == 1 || bpp == 2 || bpp == 8 || bpp == 24 || bpp == 32;

  auto fail = [&](BmpReaderError e) {
    delete[] preloadedRows_;
    preloadedRows_ = nullptr;
    return e;
  };
  if (planes != 1) return fail(BmpReaderError::BadPlanes);
  if (!validBpp) return fail(BmpReaderError::UnsupportedBpp);
  if (!(comp == 0 || (bpp == 32 && comp == 3))) return fail(BmpReaderError::UnsupportedCompression);
  if (colorsUsed > 256u) return fail(BmpReaderError::PaletteTooLarge);
  if (width <= 0 || height <= 0) return fail(BmpReaderError::BadDimensions);

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT) return fail(BmpReaderError::ImageTooLarge);

  rowBytes = (width * bpp + 31) / 32 * 4;
  if (bfOffBits + static_cast<uint32_t>(rowBytes) * static_cast<uint32_t>(height) > fileSize) {
    return fail(BmpReaderError::FileInvalid);
  }

  for (int i = 0; i < 256; i++) paletteLum[i] = static_cast<uint8_t>(i);
  if (colorsUsed > 0) {
    // Palette starts immediately after the 14+40 = 54-byte header.
    const uint8_t* pal = hdr + 54;
    if (54 + colorsUsed * 4 > fileSize) return fail(BmpReaderError::FileInvalid);
    for (uint32_t i = 0; i < colorsUsed; i++) {
      const uint8_t* rgb = pal + i * 4;
      paletteLum[i] = (77u * rgb[2] + 150u * rgb[1] + 29u * rgb[0]) >> 8;
    }
  }

  // Clean up any stale ditherers.  parseAndLoadAll is used by FB2 inline
  // image renders only, where dithering happened at decode time and we
  // never want a second pass.
  delete atkinsonDitherer;
  atkinsonDitherer = nullptr;
  delete fsDitherer;
  fsDitherer = nullptr;

  // preloadedRow() will return preloadedRows_ + bfOffBits + rowIndex * rowBytes,
  // i.e. into the pixel data section of the buffer we just slurped.
  // Repoint preloadedRows_ to start at the pixel data so existing
  // preloadedRow() math (no bfOffBits offset) Just Works.  Save the
  // original allocation pointer so the destructor can free it.
  preloadedFileStart_ = preloadedRows_;
  preloadedRows_ = preloadedRows_ + bfOffBits;
  return BmpReaderError::Ok;
}

bool Bitmap::preloadAllRows() {
  if (preloadedRows_) return true;  // already done
  if (rowBytes <= 0 || height <= 0) return false;
  // Heap allocation up front: 50 KB-ish for a 500×400 2-bit BMP.  std::nothrow
  // because the BG cache worker can fragment the heap below this threshold,
  // and falling back to row-by-row file.read is correct just slower.
  const size_t total = static_cast<size_t>(rowBytes) * static_cast<size_t>(height);
  preloadedRows_ = new (std::nothrow) uint8_t[total];
  if (!preloadedRows_) return false;
  // Single SharedBusLock-protected slurp: all pixel data in one go.
  // After a JPEG decode the SD card is in a slow recovery state where each
  // small file.read pays a 10-50 ms latency penalty — that's how we got
  // 5+ second drawBitmap renders for a 50 KB image (~366 small reads × 16 ms).
  // One big read pays the same penalty *once* instead of per row.
  snapix::spi::SharedBusLock lk;
  if (!lk || !file.seek(bfOffBits)) {
    delete[] preloadedRows_;
    preloadedRows_ = nullptr;
    return false;
  }
  if (file.read(preloadedRows_, total) != static_cast<int>(total)) {
    delete[] preloadedRows_;
    preloadedRows_ = nullptr;
    return false;
  }
  return true;
}

const uint8_t* Bitmap::preloadedRow(int rowIndex) const {
  if (!preloadedRows_ || rowIndex < 0 || rowIndex >= height) return nullptr;
  return preloadedRows_ + static_cast<size_t>(rowIndex) * static_cast<size_t>(rowBytes);
}

uint16_t Bitmap::readLE16(FsFile& f) {
  const int c0 = f.read();
  const int c1 = f.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t Bitmap::readLE32(FsFile& f) {
  const int c0 = f.read();
  const int c1 = f.read();
  const int c2 = f.read();
  const int c3 = f.read();

  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  const auto b2 = static_cast<uint8_t>(c2 < 0 ? 0 : c2);
  const auto b3 = static_cast<uint8_t>(c3 < 0 ? 0 : c3);

  return static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) | (static_cast<uint32_t>(b2) << 16) |
         (static_cast<uint32_t>(b3) << 24);
}

const char* Bitmap::errorToString(BmpReaderError err) {
  switch (err) {
    case BmpReaderError::Ok:
      return "Ok";
    case BmpReaderError::FileInvalid:
      return "FileInvalid";
    case BmpReaderError::SeekStartFailed:
      return "SeekStartFailed";
    case BmpReaderError::NotBMP:
      return "NotBMP (missing 'BM')";
    case BmpReaderError::DIBTooSmall:
      return "DIBTooSmall (<40 bytes)";
    case BmpReaderError::BadPlanes:
      return "BadPlanes (!= 1)";
    case BmpReaderError::UnsupportedBpp:
      return "UnsupportedBpp (expected 1, 2, 8, 24, or 32)";
    case BmpReaderError::UnsupportedCompression:
      return "UnsupportedCompression (expected BI_RGB or BI_BITFIELDS for 32bpp)";
    case BmpReaderError::BadDimensions:
      return "BadDimensions";
    case BmpReaderError::ImageTooLarge:
      return "ImageTooLarge (max 2048x3072)";
    case BmpReaderError::PaletteTooLarge:
      return "PaletteTooLarge";

    case BmpReaderError::SeekPixelDataFailed:
      return "SeekPixelDataFailed";
    case BmpReaderError::BufferTooSmall:
      return "BufferTooSmall";

    case BmpReaderError::OomRowBuffer:
      return "OomRowBuffer";
    case BmpReaderError::ShortReadRow:
      return "ShortReadRow";
  }
  return "Unknown";
}

BmpReaderError Bitmap::parseHeaders() {
  if (!file) return BmpReaderError::FileInvalid;
  if (!file.seek(0)) return BmpReaderError::SeekStartFailed;

  // Bulk-read the entire 54-byte fixed header (BMP file header + DIB
  // BITMAPINFOHEADER) in ONE file.read.  Replaces ~10 individual
  // readLE16/readLE32 calls that were each issuing 2-4 single-byte
  // file.read() invocations — those add up to 30+ small SD ops which,
  // during post-write SD-card recovery, take 20-50 ms each and turn a
  // ~5 ms parse into a ~1 second one.  Combined with the redundant
  // double-parse in ImageBlock::render (probe + actual), the original
  // path was 60+ small reads = 1.2 s of pure file-system overhead.
  uint8_t hdr[54];
  if (file.read(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) {
    return BmpReaderError::FileInvalid;
  }

  auto leU16 = [](const uint8_t* p) -> uint16_t {
    return static_cast<uint16_t>(p[0] | (uint16_t(p[1]) << 8));
  };
  auto leU32 = [](const uint8_t* p) -> uint32_t {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  };
  auto leI32 = [&leU32](const uint8_t* p) -> int32_t { return static_cast<int32_t>(leU32(p)); };

  // --- BMP FILE HEADER (offsets 0..13) ---
  const uint16_t bfType = leU16(hdr + 0);
  if (bfType != 0x4D42) return BmpReaderError::NotBMP;
  // hdr[2..9] = bfSize + reserved (skipped)
  bfOffBits = leU32(hdr + 10);

  // --- DIB HEADER (offsets 14..53 for the 40-byte BITMAPINFOHEADER) ---
  const uint32_t biSize = leU32(hdr + 14);
  if (biSize < 40) return BmpReaderError::DIBTooSmall;
  width = leI32(hdr + 18);
  const int32_t rawHeight = leI32(hdr + 22);
  topDown = rawHeight < 0;
  height = topDown ? -rawHeight : rawHeight;
  const uint16_t planes = leU16(hdr + 26);
  bpp = leU16(hdr + 28);
  const uint32_t comp = leU32(hdr + 30);
  // hdr[34..45] = biSizeImage + biXPelsPerMeter + biYPelsPerMeter (skipped)
  const uint32_t colorsUsed = leU32(hdr + 46);
  // hdr[50..53] = biClrImportant (skipped)

  const bool validBpp = bpp == 1 || bpp == 2 || bpp == 8 || bpp == 24 || bpp == 32;
  if (planes != 1) return BmpReaderError::BadPlanes;
  if (!validBpp) return BmpReaderError::UnsupportedBpp;
  if (!(comp == 0 || (bpp == 32 && comp == 3))) return BmpReaderError::UnsupportedCompression;
  if (colorsUsed > 256u) return BmpReaderError::PaletteTooLarge;
  if (width <= 0 || height <= 0) return BmpReaderError::BadDimensions;

  // Safety limits to prevent memory issues on ESP32
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT) {
    return BmpReaderError::ImageTooLarge;
  }

  // Pre-calculate Row Bytes to avoid doing this every row
  rowBytes = (width * bpp + 31) / 32 * 4;

  for (int i = 0; i < 256; i++) paletteLum[i] = static_cast<uint8_t>(i);
  if (colorsUsed > 0) {
    // Bulk-read the entire palette in one go (4 bytes/entry, BGRA).
    uint8_t paletteBuf[256 * 4];
    const int paletteBytes = static_cast<int>(colorsUsed * 4);
    if (file.read(paletteBuf, paletteBytes) != paletteBytes) {
      return BmpReaderError::FileInvalid;
    }
    for (uint32_t i = 0; i < colorsUsed; i++) {
      const uint8_t* rgb = paletteBuf + i * 4;
      paletteLum[i] = (77u * rgb[2] + 150u * rgb[1] + 29u * rgb[0]) >> 8;
    }
  }

  if (!file.seek(bfOffBits)) {
    return BmpReaderError::SeekPixelDataFailed;
  }

  // Clean up existing ditherers (safe if nullptr)
  delete atkinsonDitherer;
  atkinsonDitherer = nullptr;
  delete fsDitherer;
  fsDitherer = nullptr;

  // Create ditherer if enabled (only for 2-bit output)
  // Use OUTPUT dimensions for dithering (after prescaling)
  if (bpp > 2 && dithering) {
    if (USE_ATKINSON) {
      atkinsonDitherer = new AtkinsonDitherer(width);
    } else {
      fsDitherer = new FloydSteinbergDitherer(width);
    }
  }

  return BmpReaderError::Ok;
}

// packed 2bpp output, 0 = black, 1 = dark gray, 2 = light gray, 3 = white
BmpReaderError Bitmap::readRow(uint8_t* data, uint8_t* rowBuffer, int rowY) const {
  // Note: rowBuffer should be pre-allocated by the caller to size 'rowBytes'
  if (file.read(rowBuffer, rowBytes) != rowBytes) return BmpReaderError::ShortReadRow;

  prevRowY += 1;

  uint8_t* outPtr = data;
  uint8_t currentOutByte = 0;
  int bitShift = 6;
  int currentX = 0;

  // Helper lambda to pack 2bpp color into the output stream
  auto packPixel = [&](const uint8_t lum) {
    uint8_t color;
    if (atkinsonDitherer) {
      color = atkinsonDitherer->processPixel(adjustPixel(lum), currentX);
    } else if (fsDitherer) {
      color = fsDitherer->processPixel(adjustPixel(lum), currentX);
    } else {
      if (bpp > 2) {
        // Simple quantization or noise dithering
        color = quantize(adjustPixel(lum), currentX, prevRowY);
      } else {
        // do not quantize 2bpp image
        color = static_cast<uint8_t>(lum >> 6);
      }
    }
    currentOutByte |= (color << bitShift);
    if (bitShift == 0) {
      *outPtr++ = currentOutByte;
      currentOutByte = 0;
      bitShift = 6;
    } else {
      bitShift -= 2;
    }
    currentX++;
  };

  uint8_t lum;

  switch (bpp) {
    case 32: {
      const uint8_t* p = rowBuffer;
      for (int x = 0; x < width; x++) {
        lum = (77u * p[2] + 150u * p[1] + 29u * p[0]) >> 8;
        packPixel(lum);
        p += 4;
      }
      break;
    }
    case 24: {
      const uint8_t* p = rowBuffer;
      for (int x = 0; x < width; x++) {
        lum = (77u * p[2] + 150u * p[1] + 29u * p[0]) >> 8;
        packPixel(lum);
        p += 3;
      }
      break;
    }
    case 8: {
      for (int x = 0; x < width; x++) {
        packPixel(paletteLum[rowBuffer[x]]);
      }
      break;
    }
    case 2: {
      // Fast path: when the palette is the standard 4-level grayscale
      // (0=black, 0x55=dgray, 0xAA=lgray, 0xFF=white) the entire round-trip
      // collapses to identity — paletteLum[i] >> 6 == i for the four palette
      // indices, and the inner packPixel() loop's only job for 2bpp input is
      // `color = lum >> 6`.  All BMPs we generate (JpegToBmpConverter,
      // PngToBmpConverter, BitmapHelpers thumbnail) use exactly this palette,
      // so we can drop ~8 s of unpack/repack/relookup churn off a 500×400
      // image render and just memcpy the bytes through.  Detect by comparing
      // the four indices' luminance values against the canonical palette.
      const bool stdPalette = paletteLum[0] == 0x00 && paletteLum[1] == 0x55 && paletteLum[2] == 0xAA &&
                              paletteLum[3] == 0xFF && !atkinsonDitherer && !fsDitherer;
      if (stdPalette) {
        // Source row is already 2bpp packed in BMP order (MSB-first), output
        // row uses the same packing — straight copy of rowBytes bytes.  The
        // outPtr / currentOutByte / bitShift state below would normally
        // accumulate the values one pixel at a time, so we have to disable
        // the trailing flush (set bitShift to 6 to mark "no partial byte").
        const int bytesIn = (width * 2 + 7) / 8;  // packed pixel bytes (no row-end padding)
        memcpy(data, rowBuffer, bytesIn);
        bitShift = 6;
        outPtr = data + bytesIn;
        if (atkinsonDitherer) atkinsonDitherer->nextRow();
        else if (fsDitherer)  fsDitherer->nextRow();
        return BmpReaderError::Ok;
      }
      // Slow path for non-standard palettes — full unpack/repack via packPixel.
      for (int x = 0; x < width; x++) {
        lum = paletteLum[(rowBuffer[x >> 2] >> (6 - ((x & 3) << 1))) & 0x03];
        packPixel(lum);
      }
      break;
    }
    case 1: {
      for (int x = 0; x < width; x++) {
        lum = (rowBuffer[x >> 3] & (0x80 >> (x & 7))) ? 0xFF : 0x00;
        packPixel(lum);
      }
      break;
    }
    default:
      return BmpReaderError::UnsupportedBpp;
  }

  if (atkinsonDitherer)
    atkinsonDitherer->nextRow();
  else if (fsDitherer)
    fsDitherer->nextRow();

  // Flush remaining bits if width is not a multiple of 4
  if (bitShift != 6) *outPtr = currentOutByte;

  return BmpReaderError::Ok;
}

BmpReaderError Bitmap::rewindToData() const {
  if (!file.seek(bfOffBits)) {
    return BmpReaderError::SeekPixelDataFailed;
  }

  // Reset dithering state when rewinding
  prevRowY = -1;
  if (fsDitherer) fsDitherer->reset();
  if (atkinsonDitherer) atkinsonDitherer->reset();

  return BmpReaderError::Ok;
}
