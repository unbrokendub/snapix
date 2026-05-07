#pragma once

#include <functional>

class FsFile;
class Print;
class ZipFile;

class JpegToBmpConverter {
  static unsigned char jpegReadCallback(unsigned char* pBuf, unsigned char buf_size,
                                        unsigned char* pBytes_actually_read, void* pCallback_data);
  static bool jpegFileToBmpStreamInternal(class FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool quickMode = false,
                                          const std::function<bool()>& shouldAbort = nullptr);

 public:
  static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                          const std::function<bool()>& shouldAbort = nullptr);
  // Convert to 1-bit BMP (black and white only, no grays)
  static bool jpegFileTo1BitBmpStream(FsFile& jpegFile, Print& bmpOut);
  // Convert to 1-bit BMP with custom target size (for thumbnails)
  static bool jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Quick preview mode: simple threshold instead of dithering (faster but lower quality)
  static bool jpegFileToBmpStreamQuick(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                       const std::function<bool()>& shouldAbort = nullptr);
  // Header-only peek: returns true on success, fills width/height from JPEG SOF marker.
  // ~10× cheaper than a full decode — used by FB2 fast-mode image registration so the
  // page layout has correct dimensions before the BG worker decodes pixels.
  static bool peekDimensions(FsFile& jpegFile, int& outWidth, int& outHeight);

  // Tiny preview decode: picojpeg's reduce=1 mode skips AC dequant + IDCT + chroma
  // upsampling, producing a 1-pixel-per-MCU image that's 5-10× faster than a full
  // decode but at 1/8 linear resolution (8× pixelated when displayed at full size).
  // Used by the BG worker to flash a blurry preview within ~1 s before swapping in
  // the full-quality decode.  Output BMP dimensions are MCUS-per-row × MCUS-per-col.
  static bool jpegFileToBmpStreamPreview(FsFile& jpegFile, Print& bmpOut);
};
