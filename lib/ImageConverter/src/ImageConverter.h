#pragma once

#include <functional>
#include <string>

class FsFile;
class Print;

struct ImageConvertConfig {
  int maxWidth = 450;
  int maxHeight = 750;
  bool oneBit = false;
  bool quickMode = false;  // Fast preview: simple threshold instead of dithering
  // v2.0.72: write the output BMP to LittleFS (internal flash) instead of SD.
  // Cover/thumb generation hits this path because their target paths are
  // semantically LittleFS-rooted (/cache/<book_id>/cover.bmp).  Pre-2.0.72
  // the writes went to SD via SdMan even when the path looked like LittleFS,
  // which silently bloated SD storage and broke the "Clear Caches" cleanup
  // that only walks LittleFS.  Input is always SD (source images live in
  // /Books/...).
  bool outputOnLittleFs = false;
  const char* logTag = "IMG";
  std::function<bool()> shouldAbort = nullptr;
};

class ImageConverter {
 public:
  virtual ~ImageConverter() = default;
  virtual bool convert(FsFile& input, Print& output, const ImageConvertConfig& config) = 0;
  virtual const char* formatName() const = 0;
};

class ImageConverterFactory {
 public:
  // Returns appropriate converter based on file extension (or nullptr if unsupported)
  static ImageConverter* getConverter(const std::string& filePath);

  // Convenience: convert file to BMP in one call (handles file I/O)
  static bool convertToBmp(const std::string& inputPath, const std::string& outputPath,
                           const ImageConvertConfig& config = {});

  // Check if format is supported
  static bool isSupported(const std::string& filePath);
};
