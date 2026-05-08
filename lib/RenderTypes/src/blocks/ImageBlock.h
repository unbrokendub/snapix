#pragma once

#include <FS.h>  // v2.0.60: page cache moved to LittleFS — Arduino File type

#include <memory>
#include <string>

#include "Block.h"

class GfxRenderer;

class ImageBlock final : public Block {
  std::string cachedBmpPath;
  uint16_t width;
  uint16_t height;
  std::string sourceNodeId;
  std::string sourcePath;
  std::string resolvedPath;

  // Resolved-path cache: ImageBlock::render walks a 5-step fallback chain
  // (full → high → mid → low → preview), and EACH miss costs ~110-225 ms
  // on this SD card because the openFileForRead path on a missing file
  // ends up traversing the FAT directory.  The reader renders each page
  // up to 5 times per page-turn (1 BW pass + ~4 AA passes), so without
  // this cache a page with a placeholder image would burn 5 × 4 × 225ms
  // ≈ 4.5 s on file-existence probes alone.  We resolve once on the
  // first render() call and reuse the answer for every subsequent pass
  // within the same page-render cycle.  Page reload (triggered by
  // pendingBackgroundEpubRefresh_ when a new BMP stage lands)
  // reconstructs the ImageBlock from disk, so the cache is naturally
  // invalidated whenever a higher-quality stage becomes available.
  enum class ResolvedKind : uint8_t {
    Unresolved = 0,
    None = 1,         // neither full nor preview exists
    Full = 2,
    Preview = 3,
    // v2.0.48 also had High/Mid/Low values; dropped in v2.0.49 along with
    // the corresponding BMP stage files (see decodePendingImages).
  };
  mutable ResolvedKind resolvedKind_ = ResolvedKind::Unresolved;
  mutable std::string resolvedRenderPath_;

 public:
  // Defensive clamp: dimensions ≤ kMaxDim.  A pre-v2.0.20 bug read top-down BMP
  // height as int16_t (only the low 2 bytes), so a -76 pixel height was stored
  // as 65460.  Clamping at construction guarantees the same defective value
  // can't propagate through serialize → load → render after a partial cache
  // upgrade.  deserialize() additionally rejects pages whose stored dims
  // exceed this bound (see ImageBlock.cpp).
  static constexpr uint16_t kMaxDim = 2000;

  explicit ImageBlock(std::string path, const uint16_t w, const uint16_t h, std::string nodeId = {},
                      std::string src = {}, std::string resolved = {})
      : cachedBmpPath(std::move(path)),
        width(w > kMaxDim ? 0 : w),
        height(h > kMaxDim ? 0 : h),
        sourceNodeId(std::move(nodeId)),
        sourcePath(std::move(src)),
        resolvedPath(std::move(resolved)) {}
  ~ImageBlock() override = default;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return cachedBmpPath.empty(); }
  void layout(GfxRenderer& renderer) override {}

  uint16_t getWidth() const { return width; }
  uint16_t getHeight() const { return height; }
  const std::string& getCachedBmpPath() const { return cachedBmpPath; }
  const std::string& getSourceNodeId() const { return sourceNodeId; }
  const std::string& getSourcePath() const { return sourcePath; }
  const std::string& getResolvedPath() const { return resolvedPath; }

  void render(GfxRenderer& renderer, int fontId, int x, int y) const;
  bool serialize(File& file) const;
  static std::unique_ptr<ImageBlock> deserialize(File& file);
};
