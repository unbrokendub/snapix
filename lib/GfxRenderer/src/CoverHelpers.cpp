#include "CoverHelpers.h"

#include <FS.h>          // Arduino base File
#include <LittleFS.h>    // v2.0.72: cover/thumb BMPs live on internal flash for FB2/Txt/MD/HTML
#include <Bitmap.h>
#include <BitmapHelpers.h>
#include <GfxRenderer.h>
#include <ImageConverter.h>
#include <Logging.h>
#include <SDCardManager.h>

#define TAG "COVER"

#include "../../src/config.h"

namespace CoverHelpers {

namespace {

// v2.0.72: render a cover Bitmap that's already been opened + parseHeaders'd.
// Extracted so both the LittleFS and SD code paths share the same draw / refresh
// / grayscale logic.
void drawCoverAndRefresh(GfxRenderer& renderer, Bitmap& bitmap, int marginTop, int marginRight, int marginBottom,
                         int marginLeft, int& pagesUntilFullRefresh, int pagesPerRefreshValue, bool turnOffScreen,
                         bool forceHalfRefresh) {
  const int viewportWidth = renderer.getScreenWidth() - marginLeft - marginRight;
  const int viewportHeight = renderer.getScreenHeight() - marginTop - marginBottom;
  auto rect = calculateCenteredRect(bitmap.getWidth(), bitmap.getHeight(), marginLeft, marginTop, viewportWidth,
                                    viewportHeight);

  renderer.drawBitmap(bitmap, rect.x, rect.y, rect.width, rect.height);

  if (forceHalfRefresh) {
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH, turnOffScreen);
    pagesUntilFullRefresh = pagesPerRefreshValue;
  } else if (pagesPerRefreshValue == 0) {
    renderer.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);
    pagesUntilFullRefresh = 0;
  } else if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH, turnOffScreen);
    pagesUntilFullRefresh = pagesPerRefreshValue;
  } else {
    renderer.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);
    pagesUntilFullRefresh--;
  }

  if (bitmap.hasGreyscale() && renderer.storeBwBuffer()) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, rect.x, rect.y, rect.width, rect.height);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, rect.x, rect.y, rect.width, rect.height);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer(turnOffScreen);
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
  }
}

}  // namespace

bool renderCoverWithFallback(GfxRenderer& renderer, const std::string& coverPath, const std::string& previewPath,
                             int marginTop, int marginRight, int marginBottom, int marginLeft,
                             int& pagesUntilFullRefresh, int pagesPerRefreshValue, bool turnOffScreen) {
  // v2.0.72: cover BMPs may live on LittleFS (Fb2/Txt/Markdown/Html, since
  // v2.0.60+ migrated those caches) or SD (Xtc/Epub, still SD-backed).  Probe
  // both; renderCoverFromBmp handles the open dispatch.
  const bool coverOnFlash = LittleFS.exists(coverPath.c_str());
  const bool coverOnSd = !coverOnFlash && SdMan.exists(coverPath.c_str());
  if (coverOnFlash || coverOnSd) {
    return renderCoverFromBmp(renderer, coverPath, marginTop, marginRight, marginBottom, marginLeft,
                              pagesUntilFullRefresh, pagesPerRefreshValue, turnOffScreen);
  }
  const bool previewOnFlash = LittleFS.exists(previewPath.c_str());
  const bool previewOnSd = !previewOnFlash && SdMan.exists(previewPath.c_str());
  if (previewOnFlash || previewOnSd) {
    LOG_DBG(TAG, "Using preview cover (full cover not ready)");
    return renderCoverFromBmp(renderer, previewPath, marginTop, marginRight, marginBottom, marginLeft,
                              pagesUntilFullRefresh, pagesPerRefreshValue, turnOffScreen);
  }
  return false;
}

bool renderCoverFromBmp(GfxRenderer& renderer, const std::string& bmpPath, int marginTop, int marginRight,
                        int marginBottom, int marginLeft, int& pagesUntilFullRefresh, int pagesPerRefreshValue,
                        bool turnOffScreen, bool forceHalfRefresh, bool allowFastRefreshWhileOff) {
  (void)allowFastRefreshWhileOff;

  // v2.0.72: probe LittleFS first (Fb2/Txt/Markdown/Html, post-v2.0.60+ cache
  // migration) — uses the streaming Bitmap(File&) constructor added in
  // v2.0.70 so no transient ~30 KB heap slurp and no SharedBusLock contention.
  // Fall back to SD (Xtc/Epub, still SD-backed cache) if not on flash.
  File flashFile = LittleFS.open(bmpPath.c_str(), "r");
  if (flashFile) {
    Bitmap bitmap(flashFile);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) {
      flashFile.close();
      LOG_ERR(TAG, "Failed to parse cover BMP headers (flash): %s", bmpPath.c_str());
      return false;
    }
    drawCoverAndRefresh(renderer, bitmap, marginTop, marginRight, marginBottom, marginLeft, pagesUntilFullRefresh,
                        pagesPerRefreshValue, turnOffScreen, forceHalfRefresh);
    flashFile.close();
    LOG_DBG(TAG, "Rendered cover from BMP (flash)");
    return true;
  }

  FsFile sdFile;
  if (!SdMan.openFileForRead("CVR", bmpPath, sdFile)) {
    LOG_ERR(TAG, "Failed to open cover BMP: %s", bmpPath.c_str());
    return false;
  }
  Bitmap bitmap(sdFile);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    sdFile.close();
    LOG_ERR(TAG, "Failed to parse cover BMP headers (sd): %s", bmpPath.c_str());
    return false;
  }
  drawCoverAndRefresh(renderer, bitmap, marginTop, marginRight, marginBottom, marginLeft, pagesUntilFullRefresh,
                      pagesPerRefreshValue, turnOffScreen, forceHalfRefresh);
  sdFile.close();
  LOG_DBG(TAG, "Rendered cover from BMP (sd)");
  return true;
}

std::string findCoverImage(const std::string& dirPath, const std::string& baseName) {
  // Source images live in /Books/... on SD — that part of the cover pipeline
  // is unchanged.  Only the generated BMP destination moved.
  const char* extensions[] = {".jpg", ".jpeg", ".png", ".bmp"};

  // Priority 1: Image matching base filename (e.g., book.jpg for book.txt)
  for (const char* ext : extensions) {
    std::string imagePath = dirPath + "/" + baseName + ext;
    if (SdMan.exists(imagePath.c_str())) {
      LOG_DBG(TAG, "Found cover image: %s", imagePath.c_str());
      return imagePath;
    }
  }

  // Priority 2: Generic cover image in same directory
  for (const char* ext : extensions) {
    std::string imagePath = dirPath + "/cover" + ext;
    if (SdMan.exists(imagePath.c_str())) {
      LOG_DBG(TAG, "Found cover image: %s", imagePath.c_str());
      return imagePath;
    }
  }

  return "";
}

bool convertImageToBmp(const std::string& inputPath, const std::string& outputPath, const char* logTag,
                       bool use1BitDithering) {
  ImageConvertConfig config;
  config.oneBit = use1BitDithering;
  config.logTag = logTag;
  // v2.0.72: detect target filesystem by path prefix.  Cover/thumb destinations
  // for FB2/Txt/MD/HTML start with "/cache/" (LittleFS root); Xtc/Epub still
  // use SD with the same prefix because their caches haven't been migrated.
  // Disambiguate by checking which filesystem already hosts the parent dir.
  // Cheap: parent dir was created by setupCacheDir which uses the appropriate
  // FS for each content type.  We just check both `/cache/` parents.
  const auto lastSlash = outputPath.find_last_of('/');
  const std::string parentDir = (lastSlash != std::string::npos) ? outputPath.substr(0, lastSlash) : "/";
  if (LittleFS.exists(parentDir.c_str())) {
    config.outputOnLittleFs = true;
  } else {
    config.outputOnLittleFs = false;
  }
  return ImageConverterFactory::convertToBmp(inputPath, outputPath, config);
}

bool generateThumbFromCover(const std::string& coverBmpPath, const std::string& thumbBmpPath, const char* logTag) {
  // v2.0.72: detect filesystem by checking which one hosts the cover.
  const bool useFlash = LittleFS.exists(coverBmpPath.c_str());
  const bool useSd = !useFlash && SdMan.exists(coverBmpPath.c_str());
  if (!useFlash && !useSd) return false;

  // Already generated?  Check the same FS the cover lives on.
  if (useFlash && LittleFS.exists(thumbBmpPath.c_str())) return true;
  if (useSd && SdMan.exists(thumbBmpPath.c_str())) return true;

  const auto thumbTempPath = thumbBmpPath + ".tmp";
  bool ok = false;
  if (useFlash) {
    ok = bmpTo1BitBmpScaledFs(coverBmpPath.c_str(), thumbTempPath.c_str(), THUMB_WIDTH, THUMB_HEIGHT);
    if (ok) {
      if (LittleFS.rename(thumbTempPath.c_str(), thumbBmpPath.c_str())) {
        LOG_INF(logTag, "Generated thumbnail (flash)");
        return true;
      }
      LOG_ERR(logTag, "Failed to rename thumbnail %s -> %s (flash)", thumbTempPath.c_str(), thumbBmpPath.c_str());
    }
    LittleFS.remove(thumbTempPath.c_str());
  } else {
    ok = bmpTo1BitBmpScaled(coverBmpPath.c_str(), thumbTempPath.c_str(), THUMB_WIDTH, THUMB_HEIGHT);
    if (ok) {
      FsFile tempFile = SdMan.open(thumbTempPath.c_str(), O_RDWR);
      if (tempFile) {
        tempFile.rename(thumbBmpPath.c_str());
        tempFile.close();
        LOG_INF(logTag, "Generated thumbnail (sd)");
        return true;
      }
    }
    SdMan.remove(thumbTempPath.c_str());
  }
  return false;
}

}  // namespace CoverHelpers
