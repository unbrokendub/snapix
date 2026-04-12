#pragma once
#include <SdFat.h>

#include <algorithm>
#include <climits>
#include <memory>
#include <utility>
#include <vector>

#include "blocks/ImageBlock.h"
#include "blocks/TextBlock.h"

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_PageImage = 2,
};

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual PageElementTag getTag() const = 0;
  virtual void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool black = true) = 0;
  virtual bool serialize(FsFile& file) = 0;
};

// a line from a block element
class PageLine final : public PageElement {
  std::shared_ptr<TextBlock> block;

 public:
  PageLine(std::shared_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  PageElementTag getTag() const override { return TAG_PageLine; }
  const TextBlock& getTextBlock() const { return *block; }
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool black = true) override;
  bool serialize(FsFile& file) override;
  static std::unique_ptr<PageLine> deserialize(FsFile& file);
};

// an image on a page
class PageImage final : public PageElement {
  std::shared_ptr<ImageBlock> block;

 public:
 PageImage(std::shared_ptr<ImageBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  PageElementTag getTag() const override { return TAG_PageImage; }
  const ImageBlock& getImageBlock() const { return *block; }
  const std::shared_ptr<ImageBlock>& getImageBlockShared() const { return block; }
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool black = true) override;
  bool serialize(FsFile& file) override;
  static std::unique_ptr<PageImage> deserialize(FsFile& file);
};

class Page {
  mutable bool imageMetadataValid_ = false;
  mutable size_t cachedElementCount_ = 0;
  mutable bool hasImages_ = false;
  mutable int16_t imageMinX_ = 0;
  mutable int16_t imageMinY_ = 0;
  mutable int16_t imageWidth_ = 0;
  mutable int16_t imageHeight_ = 0;

  void updateImageMetadataCache() const {
    if (imageMetadataValid_ && cachedElementCount_ == elements.size()) {
      return;
    }

    bool found = false;
    int16_t minX = INT16_MAX, minY = INT16_MAX, maxX = INT16_MIN, maxY = INT16_MIN;
    for (const auto& el : elements) {
      if (el->getTag() != TAG_PageImage) continue;
      const auto& img = static_cast<const PageImage&>(*el);
      const int16_t x = img.xPos;
      const int16_t y = img.yPos;
      const int16_t right = x + img.getImageBlock().getWidth();
      const int16_t bottom = y + img.getImageBlock().getHeight();
      minX = std::min(minX, x);
      minY = std::min(minY, y);
      maxX = std::max(maxX, right);
      maxY = std::max(maxY, bottom);
      found = true;
    }

    hasImages_ = found;
    if (found) {
      imageMinX_ = minX;
      imageMinY_ = minY;
      imageWidth_ = maxX - minX;
      imageHeight_ = maxY - minY;
    } else {
      imageMinX_ = 0;
      imageMinY_ = 0;
      imageWidth_ = 0;
      imageHeight_ = 0;
    }
    cachedElementCount_ = elements.size();
    imageMetadataValid_ = true;
  }

 public:
  Page() { elements.reserve(48); }
  // the list of block index and line numbers on this page
  std::vector<std::unique_ptr<PageElement>> elements;
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool black = true) const;
  void warmGlyphs(const GfxRenderer& renderer, int fontId) const;
  bool serialize(FsFile& file) const;
  static std::unique_ptr<Page> deserialize(FsFile& file);

  bool hasImages() const {
    updateImageMetadataCache();
    return hasImages_;
  }

  // Get bounding box of all images on the page (union of image rects).
  // Coordinates are relative to page origin. Returns false if no images.
  bool getImageBoundingBox(int16_t& outX, int16_t& outY, int16_t& outW, int16_t& outH) const {
    updateImageMetadataCache();
    if (!hasImages_) return false;
    outX = imageMinX_;
    outY = imageMinY_;
    outW = imageWidth_;
    outH = imageHeight_;
    return true;
  }
};
