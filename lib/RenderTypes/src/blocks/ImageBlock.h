#pragma once

#include <SdFat.h>

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

 public:
  explicit ImageBlock(std::string path, const uint16_t w, const uint16_t h, std::string nodeId = {},
                      std::string src = {}, std::string resolved = {})
      : cachedBmpPath(std::move(path)),
        width(w),
        height(h),
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
  bool serialize(FsFile& file) const;
  static std::unique_ptr<ImageBlock> deserialize(FsFile& file);
};
