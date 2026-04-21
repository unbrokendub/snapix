#pragma once
#include <EpdFontFamily.h>
#include <SdFat.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Block.h"

// represents a block of words in the html document
class TextBlock final : public Block {
 public:
  enum BLOCK_STYLE : uint8_t {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
  };

  // Compact word reference — points into wordPool_ (no per-word heap allocation)
  struct WordData {
    uint32_t wordOffset;  // byte offset into wordPool_
    uint16_t wordLen;     // byte length (excluding null terminator)
    uint16_t xPos;
    EpdFontFamily::Style style;
  };

  // Input struct for building TextBlock from parsed words (used by ParsedText)
  struct WordInput {
    std::string word;
    uint16_t xPos;
    EpdFontFamily::Style style;
  };

 private:
  std::vector<char> wordPool_;    // contiguous storage for all word text (null-terminated)
  std::vector<WordData> wordData;
  BLOCK_STYLE style;

 public:
  // Construct from pre-built pool (deserialization, internal use)
  TextBlock(std::vector<char> pool, std::vector<WordData> data, const BLOCK_STYLE style)
      : wordPool_(std::move(pool)), wordData(std::move(data)), style(style) {}

  // Factory: build from string-based word inputs (parsing path)
  static std::shared_ptr<TextBlock> fromWords(std::vector<WordInput>& words, BLOCK_STYLE style);

  ~TextBlock() override = default;
  void setStyle(const BLOCK_STYLE style) { this->style = style; }
  BLOCK_STYLE getStyle() const { return style; }
  bool isEmpty() override { return wordData.empty(); }
  void layout(GfxRenderer& renderer) override {};
  // given a renderer works out where to break the words into lines
  void render(const GfxRenderer& renderer, int fontId, int x, int y, bool black = true) const;
  void warmGlyphs(const GfxRenderer& renderer, int fontId) const;

  /// Get null-terminated word string at index
  const char* wordCStr(size_t idx) const { return wordPool_.data() + wordData[idx].wordOffset; }

  /// Get word byte length at index
  uint16_t wordLen(size_t idx) const { return wordData[idx].wordLen; }

  /// Number of words in this block
  size_t wordCount() const { return wordData.size(); }

  /// Access word metadata at index
  const WordData& wordAt(size_t idx) const { return wordData[idx]; }

  /// Global bionic reading flag. Set before rendering pages.
  /// When true, render() splits REGULAR-styled words: bold first half, regular second half.
  static bool bionicReading;
  /// Global fake bold mode: 0=off, 1=bold (x,x+1), 2=extrabold (x-1,x,x+1).
  /// Renders BOLD/BOLD_ITALIC using REGULAR/ITALIC with multi-pass draw.
  static uint8_t fakeBold;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(FsFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(FsFile& file);
};
