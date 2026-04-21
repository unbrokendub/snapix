#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <esp_attr.h>

#define TAG "TEXT_BLOCK"

bool TextBlock::bionicReading = false;
uint8_t TextBlock::fakeBold = 0;

std::shared_ptr<TextBlock> TextBlock::fromWords(std::vector<WordInput>& words, BLOCK_STYLE style) {
  // Calculate total pool size needed
  size_t poolSize = 0;
  for (const auto& w : words) {
    poolSize += w.word.size() + 1;  // +1 for null terminator
  }

  // Build contiguous pool (single allocation)
  std::vector<char> pool;
  pool.reserve(poolSize);
  std::vector<WordData> data;
  data.reserve(words.size());

  for (auto& w : words) {
    const uint32_t offset = pool.size();
    pool.insert(pool.end(), w.word.begin(), w.word.end());
    pool.push_back('\0');
    data.push_back({offset, static_cast<uint16_t>(w.word.size()), w.xPos, w.style});
  }

  return std::make_shared<TextBlock>(std::move(pool), std::move(data), style);
}

IRAM_ATTR void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y,
                       const bool black) const {
  for (size_t i = 0; i < wordData.size(); i++) {
    const auto& wd = wordData[i];
    const char* word = wordCStr(i);

    // Fake bold: render BOLD/BOLD_ITALIC words using REGULAR/ITALIC with multi-pass draw
    if (fakeBold && (wd.style == EpdFontFamily::BOLD || wd.style == EpdFontFamily::BOLD_ITALIC)) {
      const auto drawStyle = (wd.style == EpdFontFamily::BOLD_ITALIC) ? EpdFontFamily::ITALIC : EpdFontFamily::REGULAR;
      if (fakeBold == 2) {
        // extrabold: 3× draw at x-1, x, x+1
        renderer.drawText(fontId, wd.xPos + x - 1, y, word, black, drawStyle);
        renderer.drawText(fontId, wd.xPos + x, y, word, black, drawStyle);
        renderer.drawText(fontId, wd.xPos + x + 1, y, word, black, drawStyle);
      } else {
        // bold: 2× draw at x, x+1
        renderer.drawText(fontId, wd.xPos + x, y, word, black, drawStyle);
        renderer.drawText(fontId, wd.xPos + x + 1, y, word, black, drawStyle);
      }
      continue;
    }

    if (bionicReading && wd.style == EpdFontFamily::REGULAR && wd.wordLen > 1) {
      // Count Unicode codepoints
      const unsigned char* p = reinterpret_cast<const unsigned char*>(word);
      size_t cpCount = 0;
      while (utf8NextCodepoint(&p)) cpCount++;

      if (cpCount > 1) {
        // Fake-bold first ceil(cpCount/2) codepoints by double-rendering with 1px offset.
        // This avoids loading a separate bold font file (~42KB) which fragments the heap.
        const size_t boldCount = (cpCount + 1) / 2;
        const unsigned char* split = reinterpret_cast<const unsigned char*>(word);
        for (size_t j = 0; j < boldCount; j++) utf8NextCodepoint(&split);
        const size_t boldBytes = split - reinterpret_cast<const unsigned char*>(word);

        // Draw fake-bold prefix using a stack buffer — no heap allocation.
        // Typical word prefix is <32 bytes; cap at 63 to avoid overflow.
        char prefixBuf[64];
        const size_t clampedLen = boldBytes < sizeof(prefixBuf) ? boldBytes : sizeof(prefixBuf) - 1;
        memcpy(prefixBuf, word, clampedLen);
        prefixBuf[clampedLen] = '\0';

        renderer.drawText(fontId, wd.xPos + x - 1, y, prefixBuf, black, EpdFontFamily::REGULAR);
        renderer.drawText(fontId, wd.xPos + x, y, prefixBuf, black, EpdFontFamily::REGULAR);
        renderer.drawText(fontId, wd.xPos + x + 1, y, prefixBuf, black, EpdFontFamily::REGULAR);
        const int prefixWidth = renderer.getTextAdvanceWidth(fontId, prefixBuf, EpdFontFamily::REGULAR);

        // Draw regular suffix — pointer into pool, already null-terminated.
        const char* suffix = word + boldBytes;
        renderer.drawText(fontId, wd.xPos + x + prefixWidth, y, suffix, black, EpdFontFamily::REGULAR);
        continue;
      }
      // Single codepoint: render whole word as fake-bold
      renderer.drawText(fontId, wd.xPos + x - 1, y, word, black, EpdFontFamily::REGULAR);
      renderer.drawText(fontId, wd.xPos + x, y, word, black, EpdFontFamily::REGULAR);
      renderer.drawText(fontId, wd.xPos + x + 1, y, word, black, EpdFontFamily::REGULAR);
      continue;
    }
    renderer.drawText(fontId, wd.xPos + x, y, word, black, wd.style);
  }
}

void TextBlock::warmGlyphs(const GfxRenderer& renderer, const int fontId) const {
  for (size_t i = 0; i < wordData.size(); i++) {
    auto style = wordData[i].style;
    // When fakeBold is on, BOLD/BOLD_ITALIC are drawn as REGULAR/ITALIC — warm accordingly
    if (fakeBold) {
      if (style == EpdFontFamily::BOLD) style = EpdFontFamily::REGULAR;
      else if (style == EpdFontFamily::BOLD_ITALIC) style = EpdFontFamily::ITALIC;
    }
    renderer.warmTextGlyphs(fontId, wordCStr(i), style);
  }
}

bool TextBlock::serialize(FsFile& file) const {
  // Word count
  serialization::writePod(file, static_cast<uint16_t>(wordData.size()));

  // Write words — maintain backward-compatible format (uint32_t len + bytes per word)
  for (size_t i = 0; i < wordData.size(); i++) {
    const uint32_t len = wordData[i].wordLen;
    serialization::writePod(file, len);
    file.write(reinterpret_cast<const uint8_t*>(wordCStr(i)), len);
  }

  // x positions
  for (const auto& wd : wordData) serialization::writePod(file, wd.xPos);
  // styles
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

  // Read all words into a single contiguous pool (one allocation instead of wc separate ones)
  std::vector<char> pool;
  pool.reserve(wc * 12);  // Heuristic: ~12 bytes avg per word
  std::vector<WordData> data(wc);

  for (size_t i = 0; i < wc; i++) {
    uint32_t len;
    if (!serialization::readPodChecked(file, len)) return nullptr;
    if (len > 65536) return nullptr;

    data[i].wordOffset = pool.size();
    data[i].wordLen = static_cast<uint16_t>(len);

    const size_t oldSize = pool.size();
    pool.resize(oldSize + len + 1);  // +1 for null terminator
    if (len > 0 && file.read(reinterpret_cast<uint8_t*>(&pool[oldSize]), len) != static_cast<int>(len)) {
      return nullptr;
    }
    pool[oldSize + len] = '\0';
  }
  pool.shrink_to_fit();

  // x positions
  for (auto& wd : data) {
    if (!serialization::readPodChecked(file, wd.xPos)) return nullptr;
  }
  // styles
  for (auto& wd : data) {
    if (!serialization::readPodChecked(file, wd.style)) return nullptr;
  }

  // Block style
  if (!serialization::readPodChecked(file, style)) return nullptr;

  return std::unique_ptr<TextBlock>(new TextBlock(std::move(pool), std::move(data), style));
}
