#include "ImageBlock.h"

#include <LittleFS.h>  // v2.0.53: BMPs live on internal flash, not SD

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <memory>

#define TAG "IMG_BLOCK"

void ImageBlock::render(GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  auto safeField = [](const std::string& value) -> const char* { return value.empty() ? "-" : value.c_str(); };

  auto renderPlaceholder = [&](const char* reason, const char* detail = nullptr) {
    LOG_DBG(TAG,
            "[EPUBDIAG] image fallback render node=%s src=%s resolved=%s cached=%s reason=%s%s%s render=placeholder",
            safeField(sourceNodeId), safeField(sourcePath), safeField(resolvedPath), safeField(cachedBmpPath), reason,
            detail ? " detail=" : "", detail ? detail : "");
    const char* placeholder = "Loading image...";
    const int textWidth = renderer.getTextWidth(fontId, placeholder);
    int textX = x + (static_cast<int>(width) - textWidth) / 2;
    if (textX < x) textX = x;
    // v2.0.57: anchor the text near the TOP of the reserved image box rather
    // than the geometric centre.  For tall images (height > available page
    // space) the renderer crops at the page bottom, and a y+height/2 anchor
    // lands the placeholder OFF-SCREEN — user gets a reserved-but-blank box.
    // The line-height clamp keeps single-line text visible even at the very
    // first row of a small image.
    const int lineH = renderer.getLineHeight(fontId);
    int textY = y + lineH;
    if (textY > y + static_cast<int>(height)) textY = y + static_cast<int>(height);
    if (textY < y + lineH) textY = y + lineH;
    renderer.drawText(fontId, textX, textY, placeholder, true);
  };

  if (cachedBmpPath.empty()) {
    renderPlaceholder("empty-cached-path");
    return;
  }

  // ===========================================================================
  // v2.0.53 LittleFS rendering path.
  //
  // BMPs are stored on internal flash (LittleFS), NOT on the SD card.
  // Internal flash uses a separate SPI bus from SD+display, so reads here
  // never contend with display refresh or SD post-write recovery.  Typical
  // 30 KB BMP read takes ~5-10 ms with stable timing — so we slurp+parse
  // on every render and skip the in-memory ImageRenderCache layer
  // entirely.  That cache used to pin 30+ KB of contiguous heap and
  // routinely tipped the BG worker's `isHeapCritical` watchdog into a
  // permanent deadlock (the v2.0.51-52 pathology).  With LittleFS being
  // both fast AND off-bus, the cache is no longer worth the heap cost.
  //
  // Render fallback chain: full <id>.bmp → preview <id>.preview.bmp →
  // placeholder.  Same as v2.0.49+ but reading from flash now.
  // ===========================================================================

  const auto siblingPathFromFull = [&](const char* suffix) -> std::string {
    if (cachedBmpPath.size() > 4 && cachedBmpPath.compare(cachedBmpPath.size() - 4, 4, ".bmp") == 0) {
      return cachedBmpPath.substr(0, cachedBmpPath.size() - 4) + suffix;
    }
    return std::string{};
  };

  // Resolve the highest-quality stage available — once per ImageBlock
  // instance (page reloads via pendingBackgroundEpubRefresh_ rebuild
  // ImageBlocks from cache, so a freshly-decoded full BMP gets picked up
  // on the next render cycle when resolvedKind_ resets to Unresolved).
  if (resolvedKind_ == ResolvedKind::Unresolved) {
    struct Candidate {
      ResolvedKind kind;
      std::string path;
    };
    const Candidate candidates[] = {
        {ResolvedKind::Full, cachedBmpPath},
        {ResolvedKind::Preview, siblingPathFromFull(".preview.bmp")},
    };
    resolvedKind_ = ResolvedKind::None;
    for (const auto& cand : candidates) {
      if (cand.path.empty()) continue;
      // LittleFS::exists is a cheap directory lookup (~ms).  No SPI lock,
      // no SD post-write recovery latency.
      const bool exists = LittleFS.exists(cand.path.c_str());
      LOG_DBG(TAG, "render-resolve: exists=%u path=%s", static_cast<unsigned>(exists), cand.path.c_str());
      if (!exists) continue;
      resolvedKind_ = cand.kind;
      resolvedRenderPath_ = cand.path;
      break;
    }
  }

  if (resolvedKind_ != ResolvedKind::None) {
    // v2.0.70 streaming render: open the BMP and let Bitmap stream rows
    // straight off LittleFS via the new Arduino-File constructor.  No more
    // 30 KB BMP slurp into the JPEG decode arena (or fresh heap) — drawBitmap
    // pulls one row at a time through Bitmap::readRow, which is fast on the
    // internal-flash bus (~75 µs per row * ~400 rows = ~30 ms total) and
    // never contends with the display SPI.
    //
    // The pre-v2.0.70 path slurped the whole BMP either into the persistent
    // JPEG arena (sized to fit max(scratch, BMP) via expectedBmpFileSize) or
    // into a transient `new uint8_t[fileSize]` allocation that routinely
    // OOMed on a fragmented heap.  Both costs are now zero for the render
    // step, freeing the arena to shrink to scratch-only (~13 KB) and
    // freeing ~10-15 KB of permanently-pinned RAM.
    File flashFile = LittleFS.open(resolvedRenderPath_.c_str(), "r");
    if (!flashFile) {
      LOG_ERR(TAG, "render: LittleFS.open(\"%s\") failed (exists=%u)", resolvedRenderPath_.c_str(),
              static_cast<unsigned>(LittleFS.exists(resolvedRenderPath_.c_str())));
    } else {
      const size_t fileSize = flashFile.size();
      if (fileSize < 62 || fileSize > 96 * 1024) {
        LOG_ERR(TAG, "render: BMP size out of range (size=%u path=%s)", static_cast<unsigned>(fileSize),
                resolvedRenderPath_.c_str());
        flashFile.close();
      } else {
        Bitmap stageBitmap(flashFile, true);
        const BmpReaderError parseRc = stageBitmap.parseHeaders();
        if (parseRc == BmpReaderError::Ok) {
          renderer.drawBitmap(stageBitmap, x, y, width, height);
          flashFile.close();
          return;
        }
        LOG_ERR(TAG, "render: parseHeaders failed rc=%u (%s) for %s",
                static_cast<unsigned>(parseRc), Bitmap::errorToString(parseRc),
                resolvedRenderPath_.c_str());
        flashFile.close();
      }
    }
    // Either the file vanished between resolve and read, or its header is
    // corrupt.  Fall through to placeholder; next page reload re-resolves.
    resolvedKind_ = ResolvedKind::None;
    resolvedRenderPath_.clear();
  }

  LOG_DBG(TAG, "Failed to open / parse cached BMP: %s", cachedBmpPath.c_str());
  renderPlaceholder("open-or-parse-failed");
}

bool ImageBlock::serialize(File& file) const {
  serialization::writeString(file, cachedBmpPath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  serialization::writeString(file, sourceNodeId);
  serialization::writeString(file, sourcePath);
  serialization::writeString(file, resolvedPath);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(File& file) {
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

  if (w > ImageBlock::kMaxDim || h > ImageBlock::kMaxDim) {
    LOG_ERR(TAG, "Deserialization failed: dimensions %ux%u exceed maximum (%u)", w, h,
            static_cast<unsigned>(ImageBlock::kMaxDim));
    return nullptr;
  }

  return std::unique_ptr<ImageBlock>(
      new ImageBlock(std::move(path), w, h, std::move(nodeId), std::move(src), std::move(resolved)));
}
