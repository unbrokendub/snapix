#include "ImageConverter.h"

#include <FS.h>          // Arduino base File (LittleFS output path, v2.0.72; LittleFS input, v2.0.79)
#include <LittleFS.h>    // v2.0.72: cover/thumb BMPs land on internal flash
#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <SharedSpiLock.h>

#define TAG "IMG_CONV"
#include <PngToBmpConverter.h>
#include <SDCardManager.h>

#if SNAPIX_SMOL_JPEG
#include <SmolJpeg.h>
#endif

#if SNAPIX_SMOL_PNG
#include <SmolPng.h>
#endif

namespace {

#if SNAPIX_SMOL_JPEG
// ---------------------------------------------------------------------------
// SmolJpeg adapters.  Bridge FsFile (SdFat) and fs::File (Arduino FS over
// LittleFS) onto snapix::smoljpeg::InputStream, and Print onto OutputStream.
// SmolJpeg's core lib is platform-pure; these adapters keep the Arduino /
// SdFat / LittleFS coupling localised to the integration site.
// ---------------------------------------------------------------------------
class FsFileInputStream : public snapix::smoljpeg::InputStream {
 public:
  explicit FsFileInputStream(FsFile& f) : file_(f) {}
  uint32_t length() const override {
    return static_cast<uint32_t>(file_.fileSize());
  }
  int read(uint32_t offset, uint8_t* buf, size_t len) override {
    if (!file_.seekSet(static_cast<uint64_t>(offset))) return -1;
    return file_.read(buf, len);
  }
 private:
  FsFile& file_;
};

class FlashFileInputStream : public snapix::smoljpeg::InputStream {
 public:
  explicit FlashFileInputStream(fs::File& f) : file_(f) {}
  uint32_t length() const override {
    return static_cast<uint32_t>(file_.size());
  }
  int read(uint32_t offset, uint8_t* buf, size_t len) override {
    if (!file_.seek(offset, fs::SeekSet)) return -1;
    const int got = file_.read(buf, len);
    return got < 0 ? 0 : got;
  }
 private:
  fs::File& file_;
};

class PrintOutputStream : public snapix::smoljpeg::OutputStream {
 public:
  explicit PrintOutputStream(Print& p) : print_(p) {}
  bool write(const uint8_t* data, size_t len) override {
    return print_.write(data, len) == len;
  }
 private:
  Print& print_;
};
#endif  // SNAPIX_SMOL_JPEG

#if SNAPIX_SMOL_PNG
// SmolPng has its own InputStream / OutputStream type identical in shape
// to SmolJpeg's — duplicated to keep each lib namespace-pure.
class PngFsFileInputStream : public snapix::smolpng::InputStream {
 public:
  explicit PngFsFileInputStream(FsFile& f) : file_(f) {}
  uint32_t length() const override {
    return static_cast<uint32_t>(file_.fileSize());
  }
  int read(uint32_t offset, uint8_t* buf, size_t len) override {
    if (!file_.seekSet(static_cast<uint64_t>(offset))) return -1;
    return file_.read(buf, len);
  }
 private:
  FsFile& file_;
};

class PngFlashFileInputStream : public snapix::smolpng::InputStream {
 public:
  explicit PngFlashFileInputStream(fs::File& f) : file_(f) {}
  uint32_t length() const override {
    return static_cast<uint32_t>(file_.size());
  }
  int read(uint32_t offset, uint8_t* buf, size_t len) override {
    if (!file_.seek(offset, fs::SeekSet)) return -1;
    const int got = file_.read(buf, len);
    return got < 0 ? 0 : got;
  }
 private:
  fs::File& file_;
};

class PngPrintOutputStream : public snapix::smolpng::OutputStream {
 public:
  explicit PngPrintOutputStream(Print& p) : print_(p) {}
  bool write(const uint8_t* data, size_t len) override {
    return print_.write(data, len) == len;
  }
 private:
  Print& print_;
};
#endif  // SNAPIX_SMOL_PNG

// ---------------------------------------------------------------------------
// SD-input converters (existing pipeline).  Input image lives on the SD card
// — e.g. user-picked book covers under /Books/.../cover.jpg, or pre-v2.0.73
// EPUB cache directories left behind on SD.
// ---------------------------------------------------------------------------

bool convertJpegSd(FsFile& input, Print& output, const ImageConvertConfig& config) {
  // Quick mode collapses to the same Floyd-Steinberg pipeline — the dedicated
  // quick entry point is kept for ABI parity but produces identical output
  // (see jpegFileToBmpStreamInternal comment).  Routing through size/abort.
  if (config.quickMode) {
    return JpegToBmpConverter::jpegFileToBmpStreamQuick(input, output, config.maxWidth, config.maxHeight,
                                                        config.shouldAbort);
  }

#if SNAPIX_SMOL_JPEG
  // v2.0.108 (one-decoder discipline — JPEGDEC fallback removed):
  // SmolJpeg is the only decoder when the flag is on.  See Fb2.cpp at
  // decodeImageDirect for the architectural rationale (JPEGDEC's
  // 25 KB+arena fragments the heap permanently and we've fixed
  // SmolJpeg's restart-marker bug that this fallback existed to mask).
  if (config.oneBit) {
    FsFileInputStream sin(input);
    PrintOutputStream sout(output);
    const auto status = snapix::smoljpeg::decodeTo1BitBmp(
        sin, sout, config.maxWidth, config.maxHeight, nullptr /* TODO: shouldAbort thunk */);
    if (status == snapix::smoljpeg::Status::Ok) return true;
    LOG_INF(config.logTag, "SmolJpeg(SD) decode failed (%s) — no fallback (one-decoder discipline)",
            snapix::smoljpeg::statusToString(status));
    return false;
  }
  // 2-bit path falls through to legacy below — kept for non-1bit callers
  // (preview generation).  Will revisit when SmolJpeg gains 2-bit output.
#endif

  if (config.maxWidth == 450 && config.maxHeight == 750 && !config.shouldAbort) {
    return config.oneBit ? JpegToBmpConverter::jpegFileTo1BitBmpStream(input, output)
                         : JpegToBmpConverter::jpegFileToBmpStream(input, output);
  }
  return config.oneBit
             ? JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(input, output, config.maxWidth, config.maxHeight)
             : JpegToBmpConverter::jpegFileToBmpStreamWithSize(input, output, config.maxWidth, config.maxHeight,
                                                               config.shouldAbort);
}

bool convertPngSd(FsFile& input, Print& output, const ImageConvertConfig& config) {
  if (config.quickMode) {
    return PngToBmpConverter::pngFileToBmpStreamQuick(input, output, config.maxWidth, config.maxHeight,
                                                      config.shouldAbort);
  }
#if SNAPIX_SMOL_PNG
  // SmolPng one-pass path: chunk parse → uzlib deflate → filter inversion
  // → RGB→luma → Floyd-Steinberg dither → 1-bit BMP, no intermediate
  // grayscale buffer.  Falls back to pngle on any decode failure.
  // NOTE: snapix v2 always emits 2-bit PNG output; SmolPng emits 1-bit.
  // We honour `oneBit` here for symmetry with the JPEG path even though
  // PNG callers normally request 2-bit — Phase 6 alpha; revisit caller
  // contracts in Phase 6f.
  if (config.oneBit) {
    PngFsFileInputStream sin(input);
    PngPrintOutputStream sout(output);
    const auto status = snapix::smolpng::decodeTo1BitBmp(
        sin, sout, config.maxWidth, config.maxHeight, nullptr /* TODO: abort thunk */);
    if (status == snapix::smolpng::Status::Ok) return true;
    LOG_INF(config.logTag, "SmolPng(SD) decode failed (%s); falling back to pngle",
            snapix::smolpng::statusToString(status));
    input.seekSet(0);
  }
#endif
  // PNG converter always produces 2-bit output (oneBit flag is ignored —
  // unlike JPEG, PNG dithering doesn't have a 1-bit variant).  Thumbnails
  // are slightly larger but render at the same speed on the e-paper panel.
  return PngToBmpConverter::pngFileToBmpStreamWithSize(input, output, config.maxWidth, config.maxHeight,
                                                       config.shouldAbort);
}

bool convertBmpSd(FsFile& input, Print& output, const ImageConvertConfig& /*config*/) {
  uint8_t buffer[512];
  while (input.available()) {
    const int readResult = input.read(buffer, sizeof(buffer));
    if (readResult <= 0) {
      return false;
    }
    const size_t bytesRead = static_cast<size_t>(readResult);
    if (output.write(buffer, bytesRead) != bytesRead) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// v2.0.79: LittleFS-input converters.  Post-v2.0.73 every EPUB cache lives
// on internal flash, including the temp files written between ZIP extract
// and BMP conversion (e.g. /cache/epub_<hash>/images/.tmp_<srcHash>.jpg).
// Pre-2.0.79 the converter blindly tried SD.openFileForRead on those paths,
// which always failed — every EPUB inline image silently fell back to a
// placeholder, every newly-loaded EPUB cover/thumb regenerated nothing.
// ---------------------------------------------------------------------------

bool convertJpegLittleFs(fs::File& input, Print& output, const ImageConvertConfig& config) {
#if SNAPIX_SMOL_JPEG
  // v2.0.108 (one-decoder discipline — JPEGDEC fallback removed):
  // see Fb2.cpp decodeImageDirect for rationale.  1-bit oneBit path
  // goes through SmolJpeg only; failure → return false, no JPEGDEC
  // fragmentation.  2-bit path (oneBit=false) still uses legacy
  // until SmolJpeg gains 2-bit output (Phase 6f).
  if (config.oneBit) {
    FlashFileInputStream sin(input);
    PrintOutputStream    sout(output);
    const auto status = snapix::smoljpeg::decodeTo1BitBmp(
        sin, sout, config.maxWidth, config.maxHeight, nullptr /* TODO: shouldAbort thunk */);
    if (status == snapix::smoljpeg::Status::Ok) return true;
    LOG_INF(config.logTag, "SmolJpeg(flash) decode failed (%s) — no fallback (one-decoder discipline)",
            snapix::smoljpeg::statusToString(status));
    return false;
  }
  // 2-bit fall-through (rare — only archival-quality non-reader callers)
#endif
  return JpegToBmpConverter::jpegLittleFsFileToBmpStreamWithSize(input, output, config.maxWidth, config.maxHeight,
                                                                   config.shouldAbort);
}

bool convertPngLittleFs(fs::File& input, Print& output, const ImageConvertConfig& config) {
  if (config.quickMode) {
    return PngToBmpConverter::pngLittleFsFileToBmpStreamQuick(input, output, config.maxWidth, config.maxHeight,
                                                                config.shouldAbort);
  }
#if SNAPIX_SMOL_PNG
  if (config.oneBit) {
    PngFlashFileInputStream sin(input);
    PngPrintOutputStream    sout(output);
    const auto status = snapix::smolpng::decodeTo1BitBmp(
        sin, sout, config.maxWidth, config.maxHeight, nullptr /* TODO: abort thunk */);
    if (status == snapix::smolpng::Status::Ok) return true;
    LOG_INF(config.logTag, "SmolPng(flash) decode failed (%s); falling back to pngle",
            snapix::smolpng::statusToString(status));
    input.seek(0, fs::SeekSet);
  }
#endif
  return PngToBmpConverter::pngLittleFsFileToBmpStreamWithSize(input, output, config.maxWidth, config.maxHeight,
                                                                 config.shouldAbort);
}

bool convertBmpLittleFs(fs::File& input, Print& output, const ImageConvertConfig& /*config*/) {
  // fs::File::read returns size_t with 0 meaning EOF — no negative-error
  // sentinel to distinguish from a clean end.  Loop until available() drops
  // to zero, then exit.
  uint8_t buffer[512];
  while (input.available() > 0) {
    const size_t bytesRead = input.read(buffer, sizeof(buffer));
    if (bytesRead == 0) break;  // EOF (or unexpected zero-read)
    if (output.write(buffer, bytesRead) != bytesRead) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Format detection.
// ---------------------------------------------------------------------------
enum class ImageFormat { Jpeg, Png, Bmp, Unsupported };

ImageFormat detectFormat(const std::string& filePath) {
  if (FsHelpers::isJpegFile(filePath)) return ImageFormat::Jpeg;
  if (FsHelpers::isPngFile(filePath)) return ImageFormat::Png;
  if (FsHelpers::isBmpFile(filePath)) return ImageFormat::Bmp;
  return ImageFormat::Unsupported;
}

const char* formatName(ImageFormat fmt) {
  switch (fmt) {
    case ImageFormat::Jpeg: return "JPEG";
    case ImageFormat::Png:  return "PNG";
    case ImageFormat::Bmp:  return "BMP";
    default:                return "?";
  }
}

}  // namespace

// Legacy ImageConverter virtual hierarchy (kept for ABI parity in case any
// out-of-tree caller still resolves a converter by extension and feeds it an
// already-open FsFile).  In-tree, every caller goes through
// `convertToBmp(inputPath, outputPath, ...)`.
namespace {
class JpegImageConverter : public ImageConverter {
 public:
  bool convert(FsFile& input, Print& output, const ImageConvertConfig& config) override {
    return convertJpegSd(input, output, config);
  }
  const char* formatName() const override { return "JPEG"; }
};
class PngImageConverter : public ImageConverter {
 public:
  bool convert(FsFile& input, Print& output, const ImageConvertConfig& config) override {
    return convertPngSd(input, output, config);
  }
  const char* formatName() const override { return "PNG"; }
};
class BmpImageConverter : public ImageConverter {
 public:
  bool convert(FsFile& input, Print& output, const ImageConvertConfig& config) override {
    return convertBmpSd(input, output, config);
  }
  const char* formatName() const override { return "BMP"; }
};

JpegImageConverter jpegConverter;
PngImageConverter pngConverter;
BmpImageConverter bmpConverter;
}  // namespace

ImageConverter* ImageConverterFactory::getConverter(const std::string& filePath) {
  switch (detectFormat(filePath)) {
    case ImageFormat::Jpeg: return &jpegConverter;
    case ImageFormat::Png:  return &pngConverter;
    case ImageFormat::Bmp:  return &bmpConverter;
    default:                return nullptr;
  }
}

// v2.0.79: auto-detect input filesystem.  Probe LittleFS first — since
// v2.0.73 the vast majority of inputs handed to this entry point live on
// internal flash (EPUB chapter image temp files, EPUB cover/thumb temp
// files).  Fall back to SD for user-picked image files in /Books and
// any other SD-rooted source.
//
// Why probe instead of relying on a flag:
//   * Adding `inputOnLittleFs` to ImageConvertConfig would force every call
//     site to keep its filesystem knowledge in sync with the cache layout.
//     v2.0.73 already missed updating the converter; a second missed site
//     would resurface the same silent placeholder bug.
//   * LittleFS.exists() is cheap (~200 µs uncached) compared to the JPEG
//     decode that follows.  Doing it once per conversion is in the noise.
//   * The probe is unambiguous because LittleFS and SD have disjoint
//     mountpoints in practice: LittleFS is mounted as the root of the
//     internal-flash partition (paths like `/cache/...`), SD is mounted via
//     SDFat under SdMan (paths like `/Books/...`).  A path lives in
//     exactly one filesystem.
bool ImageConverterFactory::convertToBmp(const std::string& inputPath, const std::string& outputPath,
                                         const ImageConvertConfig& config) {
  const ImageFormat format = detectFormat(inputPath);
  if (format == ImageFormat::Unsupported) {
    LOG_ERR(config.logTag, "Unsupported image format: %s", inputPath.c_str());
    return false;
  }

  const bool inputOnLittleFs = LittleFS.exists(inputPath.c_str());

  // SharedBusLock guards the SD bus.  Only acquire when at least one side of
  // the conversion touches SD — otherwise we'd needlessly block the e-ink
  // display thread.  LittleFS sits on a different SPI controller.
  const bool sdInvolved = !inputOnLittleFs || !config.outputOnLittleFs;
  snapix::spi::SharedBusLock busLock;
  if (sdInvolved && !busLock) {
    LOG_ERR(config.logTag, "Shared SPI lock unavailable for image conversion");
    return false;
  }

  // Open output first — failing here saves us opening the input only to
  // throw it away.  LittleFS and SD use different File types but both
  // implement Print, so the converter doesn't care which lands here.
  bool success = false;
  File flashOutput;
  FsFile sdOutput;
  Print* output = nullptr;
  if (config.outputOnLittleFs) {
    flashOutput = LittleFS.open(outputPath.c_str(), "w");
    if (!flashOutput) {
      LOG_ERR(config.logTag, "Failed to create LittleFS output file: %s", outputPath.c_str());
      return false;
    }
    output = &flashOutput;
  } else {
    if (!SdMan.openFileForWrite(config.logTag, outputPath, sdOutput)) {
      LOG_ERR(config.logTag, "Failed to create SD output file: %s", outputPath.c_str());
      return false;
    }
    output = &sdOutput;
  }

  // Dispatch on (input filesystem, format).  Each branch opens its own
  // file handle in the matching FS API, hands it to the matching converter,
  // and closes on the way out.  Keep the close calls in the same branch
  // they were opened in to avoid type-mismatch noise.
  if (inputOnLittleFs) {
    File flashInput = LittleFS.open(inputPath.c_str(), "r");
    if (!flashInput) {
      LOG_ERR(config.logTag, "Failed to open LittleFS input file: %s", inputPath.c_str());
    } else {
      switch (format) {
        case ImageFormat::Jpeg: success = convertJpegLittleFs(flashInput, *output, config); break;
        case ImageFormat::Png:  success = convertPngLittleFs(flashInput, *output, config); break;
        case ImageFormat::Bmp:  success = convertBmpLittleFs(flashInput, *output, config); break;
        default:                break;
      }
      flashInput.close();
    }
  } else {
    FsFile sdInput;
    if (!SdMan.openFileForRead(config.logTag, inputPath, sdInput)) {
      LOG_ERR(config.logTag, "Failed to open input file: %s", inputPath.c_str());
    } else {
      switch (format) {
        case ImageFormat::Jpeg: success = convertJpegSd(sdInput, *output, config); break;
        case ImageFormat::Png:  success = convertPngSd(sdInput, *output, config); break;
        case ImageFormat::Bmp:  success = convertBmpSd(sdInput, *output, config); break;
        default:                break;
      }
      sdInput.close();
    }
  }

  // Close output + clean up partial files on failure.
  if (config.outputOnLittleFs) {
    flashOutput.close();
    if (!success) {
      LittleFS.remove(outputPath.c_str());
      LOG_ERR(config.logTag, "Failed to convert %s to BMP", formatName(format));
    } else {
      LOG_INF(config.logTag, "Converted %s to BMP (flash, src=%s): %s", formatName(format),
              inputOnLittleFs ? "flash" : "sd", outputPath.c_str());
    }
  } else {
    sdOutput.close();
    if (!success) {
      SdMan.remove(outputPath.c_str());
      LOG_ERR(config.logTag, "Failed to convert %s to BMP", formatName(format));
    } else {
      LOG_INF(config.logTag, "Converted %s to BMP (sd, src=%s): %s", formatName(format),
              inputOnLittleFs ? "flash" : "sd", outputPath.c_str());
    }
  }

  return success;
}

bool ImageConverterFactory::isSupported(const std::string& filePath) { return FsHelpers::isImageFile(filePath); }

// v2.0.89: SmolJpeg-routed peek for the async-decode dimension-only path.
//
// The legacy `JpegToBmpConverter::peekDimensionsLittleFs` lazily allocates a
// ~25 KB shared JPEGDEC instance; on the ESP32-C3 once a chapter has burned
// through a couple of inline images and the heap has fragmented below
// ~25 KB largest-free-block, every subsequent peek fails OOM and the sync
// fallback also OOMs (same singleton).  SmolJpeg's `peekDimensions` streams
// from the file with a 256-byte sliding window (no large heap alloc), so it
// keeps working in low-heap conditions and never trips the OOM cascade.
//
// When `SNAPIX_SMOL_JPEG` is off (e.g. the default env) we fall straight
// through to the JPEGDEC implementation for source-level parity.
bool ImageConverterFactory::peekJpegDimensionsLittleFs(const std::string& path, int& outWidth, int& outHeight) {
  outWidth = 0;
  outHeight = 0;

#if SNAPIX_SMOL_JPEG
  {
    fs::File peekFile = LittleFS.open(path.c_str(), "r");
    if (!peekFile) return false;
    FlashFileInputStream sin(peekFile);
    uint16_t w = 0, h = 0;
    const auto status = snapix::smoljpeg::peekDimensions(sin, w, h);
    peekFile.close();
    if (status == snapix::smoljpeg::Status::Ok && w > 0 && h > 0) {
      outWidth = w;
      outHeight = h;
      return true;
    }
    // SmolJpeg parser bailed (truncated header, unsupported feature, etc.) —
    // fall through to the JPEGDEC peek which is more permissive about exotic
    // marker layouts at the cost of the ~25 KB workspace alloc.
  }
#endif

  fs::File peekFile = LittleFS.open(path.c_str(), "r");
  if (!peekFile) return false;
  const bool ok = JpegToBmpConverter::peekDimensionsLittleFs(peekFile, outWidth, outHeight);
  peekFile.close();
  return ok;
}
