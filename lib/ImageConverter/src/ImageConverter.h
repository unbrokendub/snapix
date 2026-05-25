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
  // that only walks LittleFS.
  //
  // v2.0.79: input filesystem is now auto-detected inside convertToBmp by
  // probing LittleFS.exists() before falling back to SD.  Post-v2.0.73 the
  // majority of inputs live on flash (EPUB chapter image temp files, EPUB
  // cover/thumb temp files); pre-2.0.79 they all silently failed because
  // the converter hard-coded SDFat reads.  No flag here — see the comment
  // above convertToBmp for the auto-detect rationale.
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

  // v2.0.89: cheap peek of a LittleFS-backed JPEG's source dimensions for the
  // async-decode dimension-only path in ChapterHtmlSlimParser.
  //
  // When `SNAPIX_SMOL_JPEG` is on this routes through `SmolJpeg::peekDimensions`
  // (streaming, ~300 B transient RAM).  Otherwise it falls back to the legacy
  // `JpegToBmpConverter::peekDimensionsLittleFs` which allocates a ~25 KB
  // JPEGDEC workspace and fails `OOM allocating shared workspace (~25 KB)`
  // once the heap has fragmented below that watermark (the symptom seen on
  // image/2.jpg, image/3.jpg, … in heavy chapters once the JPEGDEC singleton
  // has been released to recover headroom).
  static bool peekJpegDimensionsLittleFs(const std::string& path, int& outWidth, int& outHeight);
};
