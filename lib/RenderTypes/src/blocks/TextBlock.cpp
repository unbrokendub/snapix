#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#define TAG "TEXT_BLOCK"

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y,
                       const bool black) const {
  for (const auto& wd : wordData) {
    renderer.drawText(fontId, wd.xPos + x, y, wd.word.c_str(), black, wd.style);
  }
}

void TextBlock::warmGlyphs(const GfxRenderer& renderer, const int fontId) const {
  for (const auto& wd : wordData) {
    renderer.warmTextGlyphs(fontId, wd.word.c_str(), wd.style);
  }
}

bool TextBlock::serialize(FsFile& file) const {
  // Word count
  serialization::writePod(file, static_cast<uint16_t>(wordData.size()));

  // Write words, then xpos, then styles (maintains backward-compatible format)
  for (const auto& wd : wordData) serialization::writeString(file, wd.word);
  for (const auto& wd : wordData) serialization::writePod(file, wd.xPos);
  for (const auto& wd : wordData) serialization::writePod(file, wd.style);

  // Block style
  serialization::writePod(file, style);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  BLOCK_STYLE style;

  // Word count
  if (!serialization::readPodChecked(file, wc)) {
    return nullptr;
  }

  // Sanity check: prevent allocation of unreasonably large vectors (max 10000 words per block)
  if (wc > 10000) {
    LOG_ERR(TAG, "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  // Read directly into the final vector while preserving the on-disk format
  // (all words first, then x positions, then styles).
  std::vector<WordData> data(wc);

  for (auto& wd : data) {
    if (!serialization::readString(file, wd.word)) {
      return nullptr;
    }
  }
  for (auto& wd : data) {
    if (!serialization::readPodChecked(file, wd.xPos)) {
      return nullptr;
    }
  }
  for (auto& wd : data) {
    if (!serialization::readPodChecked(file, wd.style)) {
      return nullptr;
    }
  }

  // Block style
  if (!serialization::readPodChecked(file, style)) {
    return nullptr;
  }

  return std::unique_ptr<TextBlock>(new TextBlock(std::move(data), style));
}
