#pragma once

#include <functional>

class FsFile;
class Print;

namespace fs {
class File;
}  // namespace fs

class PngToBmpConverter {
 public:
  static bool pngFileToBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                         const std::function<bool()>& shouldAbort = nullptr);
  // Quick preview mode: simple threshold instead of dithering (faster but lower quality)
  static bool pngFileToBmpStreamQuick(FsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                      const std::function<bool()>& shouldAbort = nullptr);

  // v2.0.79: LittleFS-backed inputs.  EPUB chapter inline images and EPUB
  // cover/thumb temp files live on internal flash post-v2.0.73; the SD-only
  // entry points above can't reach them.  Pipeline and output BMP are
  // identical — only the file-handle type and per-read locking differ.
  static bool pngLittleFsFileToBmpStreamWithSize(fs::File& pngFile, Print& bmpOut, int targetMaxWidth,
                                                  int targetMaxHeight,
                                                  const std::function<bool()>& shouldAbort = nullptr);
  static bool pngLittleFsFileToBmpStreamQuick(fs::File& pngFile, Print& bmpOut, int targetMaxWidth,
                                               int targetMaxHeight,
                                               const std::function<bool()>& shouldAbort = nullptr);

  // Header-only peek: parses the IHDR chunk for width / height without
  // decompressing pixel data — used by FB2 fast-mode image registration.
  static bool peekDimensions(FsFile& pngFile, int& outWidth, int& outHeight);
};
