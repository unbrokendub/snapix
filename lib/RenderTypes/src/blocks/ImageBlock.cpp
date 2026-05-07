#include "ImageBlock.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SDCardManager.h>
#include <Serialization.h>

#define TAG "IMG_BLOCK"

void ImageBlock::render(GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  auto safeField = [](const std::string& value) -> const char* { return value.empty() ? "-" : value.c_str(); };

  auto renderPlaceholder = [&](const char* reason, const char* detail = nullptr) {
    LOG_ERR(TAG,
            "[EPUBDIAG] image fallback render node=%s src=%s resolved=%s cached=%s reason=%s%s%s render=placeholder",
            safeField(sourceNodeId), safeField(sourcePath), safeField(resolvedPath), safeField(cachedBmpPath), reason,
            detail ? " detail=" : "", detail ? detail : "");
    const char* placeholder = "[Image]";
    const int textWidth = renderer.getTextWidth(fontId, placeholder);
    int textX = x + (static_cast<int>(width) - textWidth) / 2;
    if (textX < x) textX = x;
    const int textY = y + height / 2;
    renderer.drawText(fontId, textX, textY, placeholder, true);
  };

  if (cachedBmpPath.empty()) {
    renderPlaceholder("empty-cached-path");
    return;
  }

  // Render fallback chain: full <id>.bmp → preview <id>.preview.bmp →
  // placeholder.  Both opening AND parsing failures of the full BMP
  // demote to preview, since a corrupt full BMP from a v2.0.34-era
  // non-atomic write would otherwise pin the page on the placeholder
  // even though a usable preview is sitting next to it on disk.
  const auto previewPathFromFull = [&]() -> std::string {
    if (cachedBmpPath.size() > 4 && cachedBmpPath.compare(cachedBmpPath.size() - 4, 4, ".bmp") == 0) {
      return cachedBmpPath.substr(0, cachedBmpPath.size() - 4) + ".preview.bmp";
    }
    return std::string{};
  };

  std::string activePath = cachedBmpPath;
  bool isPreview = false;
  FsFile bmpFile;
  bool openedOk = false;

  if (SdMan.openFileForRead("IMB", activePath, bmpFile)) {
    Bitmap probe(bmpFile, true);
    const BmpReaderError err = probe.parseHeaders();
    if (err == BmpReaderError::Ok) {
      openedOk = true;
    } else {
      LOG_ERR(TAG, "BMP parse error (full): %s — trying preview fallback", Bitmap::errorToString(err));
      bmpFile.close();
    }
  }

  if (!openedOk) {
    const std::string previewPath = previewPathFromFull();
    if (!previewPath.empty() && SdMan.openFileForRead("IMB", previewPath, bmpFile)) {
      Bitmap probe(bmpFile, true);
      if (probe.parseHeaders() == BmpReaderError::Ok) {
        activePath = previewPath;
        isPreview = true;
        openedOk = true;
      } else {
        bmpFile.close();
      }
    }
  }

  if (!openedOk) {
    LOG_ERR(TAG, "Failed to open / parse cached BMP: %s", cachedBmpPath.c_str());
    renderPlaceholder("open-or-parse-failed");
    return;
  }

  // Re-parse on the now-validated handle (the probe's parseHeaders mutates
  // the file position, so the second pass via Bitmap is what drawBitmap
  // streams from).
  if (!bmpFile.seek(0)) {
    bmpFile.close();
    renderPlaceholder("seek-failed");
    return;
  }
  Bitmap bitmap(bmpFile, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    bmpFile.close();
    renderPlaceholder("bmp-parse-failed-on-rewind");
    return;
  }
  (void)isPreview;

  renderer.drawBitmap(bitmap, x, y, width, height);
  bmpFile.close();
}

bool ImageBlock::serialize(FsFile& file) const {
  serialization::writeString(file, cachedBmpPath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  serialization::writeString(file, sourceNodeId);
  serialization::writeString(file, sourcePath);
  serialization::writeString(file, resolvedPath);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile& file) {
  std::string path;
  std::string nodeId;
  std::string src;
  std::string resolved;
  uint16_t w, h;

  if (!serialization::readString(file, path) || !serialization::readPodChecked(file, w) ||
      !serialization::readPodChecked(file, h) || !serialization::readString(file, nodeId) ||
      !serialization::readString(file, src) || !serialization::readString(file, resolved)) {
    LOG_ERR(TAG, "Deserialization failed: couldn't read data");
    return nullptr;
  }

  // Sanity check: prevent unreasonable dimensions from corrupted data.
  // Pre-v2.0.20 caches stored 65460 (= -76 cast to uint16_t) for top-down BMPs;
  // the bumped CACHE_FILE_VERSION should already invalidate them, but guard
  // here too in case any slip through during a partial upgrade.
  if (w > ImageBlock::kMaxDim || h > ImageBlock::kMaxDim) {
    LOG_ERR(TAG, "Deserialization failed: dimensions %ux%u exceed maximum (%u)", w, h,
            static_cast<unsigned>(ImageBlock::kMaxDim));
    return nullptr;
  }

  return std::unique_ptr<ImageBlock>(
      new ImageBlock(std::move(path), w, h, std::move(nodeId), std::move(src), std::move(resolved)));
}
