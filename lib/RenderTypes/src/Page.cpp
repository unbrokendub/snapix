#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>
#include <vector>

#if __has_include(<esp_attr.h>)
#include <esp_attr.h>
#endif
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

#define TAG "PAGE"

IRAM_ATTR void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset, const bool black) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset, black);
}

bool PageLine::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  if (!tb) {
    LOG_ERR(TAG, "Deserialization failed: TextBlock is null");
    return nullptr;
  }
  return std::unique_ptr<PageLine>(new PageLine(std::move(tb), xPos, yPos));
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                       const bool black) {
  if (!black) {
    renderer.clearArea(xPos + xOffset, yPos + yOffset, block->getWidth(), block->getHeight(), 0xFF);
  }
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  return block->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto ib = ImageBlock::deserialize(file);
  if (!ib) {
    return nullptr;
  }
  return std::unique_ptr<PageImage>(new PageImage(std::move(ib), xPos, yPos));
}

IRAM_ATTR void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                  const bool black) const {
  for (auto& element : elements) {
    element->render(renderer, fontId, xOffset, yOffset, black);
  }
}

void Page::warmGlyphs(const GfxRenderer& renderer, const int fontId) const {
  // Batch glyph warming across the entire page.  Per-line warming caused
  // LRU thrashing in the external font cache (~80 entries) when Cyrillic /
  // CJK pages had 100+ unique codepoints split across many words; cold
  // first-render took 15s.  Collecting all codepoints once and dispatching
  // a single batch per style amortises sort/dedup and lets preloadGlyphs
  // see the full page set so the LRU never evicts a glyph the page needs.
  //
  // Codepoints are bucketed per style (REGULAR / BOLD / ITALIC / BOLD_ITALIC)
  // because each style has its own glyph data.  When fakeBold is enabled,
  // BOLD/BOLD_ITALIC remap to REGULAR/ITALIC respectively.
  std::vector<uint32_t> bucket[4];  // index by EpdFontFamily::Style enum (0..3)
  for (auto& v : bucket) v.reserve(64);

  for (const auto& element : elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& tb = static_cast<const PageLine&>(*element).getTextBlock();
    for (size_t i = 0; i < tb.wordCount(); i++) {
      const auto& wd = tb.wordAt(i);
      auto style = wd.style;
      if (TextBlock::fakeBold) {
        if (style == EpdFontFamily::BOLD) style = EpdFontFamily::REGULAR;
        else if (style == EpdFontFamily::BOLD_ITALIC) style = EpdFontFamily::ITALIC;
      }
      const int idx = static_cast<int>(style);
      if (idx < 0 || idx >= 4) continue;
      const char* text = tb.wordCStr(i);
      const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text);
      uint32_t cp;
      while ((cp = utf8NextCodepoint(&ptr))) {
        bucket[idx].push_back(cp);
      }
    }
  }

  for (int s = 0; s < 4; s++) {
    if (bucket[s].empty()) continue;
    std::sort(bucket[s].begin(), bucket[s].end());
    bucket[s].erase(std::unique(bucket[s].begin(), bucket[s].end()), bucket[s].end());
    renderer.warmCodepointsBatch(fontId, bucket[s].data(), bucket[s].size(),
                                  static_cast<EpdFontFamily::Style>(s));
  }
}

bool Page::serialize(FsFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));
    if (!el->serialize(file)) {
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  auto page = std::unique_ptr<Page>(new Page());

  // Max elements per page - prevents memory exhaustion from corrupted cache
  constexpr uint16_t MAX_PAGE_ELEMENTS = 500;

  uint16_t count;
  serialization::readPod(file, count);

  // Validate element count to prevent memory exhaustion
  if (count > MAX_PAGE_ELEMENTS) {
    LOG_ERR(TAG, "Element count %u exceeds limit %u", count, MAX_PAGE_ELEMENTS);
    return nullptr;
  }

  page->elements.reserve(count);

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        LOG_ERR(TAG, "Deserialization failed: PageLine is null");
        return nullptr;
      }
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) {
        LOG_ERR(TAG, "Deserialization failed: PageImage is null");
        return nullptr;
      }
      page->elements.push_back(std::move(pi));
    } else {
      LOG_ERR(TAG, "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  return page;
}
