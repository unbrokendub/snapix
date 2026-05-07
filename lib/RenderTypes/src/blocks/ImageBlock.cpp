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

  // Single-parse open + render.  v2.0.39 was doing TWO parseHeaders per
  // image render (probe to validate, then re-parse for actual streaming),
  // which during post-write SD recovery cost ~600 ms each.  parseHeaders
  // now leaves the file cursor at bfOffBits (start of pixel data) ready
  // for drawBitmap's readRow/preload, so we just keep the validated
  // Bitmap and hand it to the renderer directly.
  FsFile bmpFile;

  auto tryOpenAndParse = [](const std::string& path, FsFile& outFile, Bitmap& outBitmap) -> bool {
    if (!SdMan.openFileForRead("IMB", path, outFile)) return false;
    if (outBitmap.parseHeaders() != BmpReaderError::Ok) {
      outFile.close();
      return false;
    }
    return true;
  };

  // Try full BMP first.
  Bitmap fullBitmap(bmpFile, true);
  if (tryOpenAndParse(cachedBmpPath, bmpFile, fullBitmap)) {
    renderer.drawBitmap(fullBitmap, x, y, width, height);
    bmpFile.close();
    return;
  }

  // Fallback: preview BMP (post-v2.0.27 BG decode writes one of these
  // before the full decode finishes).
  const std::string previewPath = previewPathFromFull();
  if (!previewPath.empty()) {
    Bitmap previewBitmap(bmpFile, true);
    if (tryOpenAndParse(previewPath, bmpFile, previewBitmap)) {
      renderer.drawBitmap(previewBitmap, x, y, width, height);
      bmpFile.close();
      return;
    }
  }

  LOG_ERR(TAG, "Failed to open / parse cached BMP: %s", cachedBmpPath.c_str());
  renderPlaceholder("open-or-parse-failed");
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
