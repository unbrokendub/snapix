#include "ParsedText.h"

#include <GfxRenderer.h>
#include <Hyphenation.h>
#include <Logging.h>
#include <Utf8.h>

#define TAG "TEXT"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

// Knuth-Plass algorithm constants
constexpr float INFINITY_PENALTY = 10000.0f;
constexpr float LINE_PENALTY = 50.0f;
constexpr size_t MAX_GREEDY_WORDS_PER_LINE = 64;

// Soft hyphen (U+00AD) as UTF-8 bytes
constexpr unsigned char SOFT_HYPHEN_BYTE1 = 0xC2;
constexpr unsigned char SOFT_HYPHEN_BYTE2 = 0xAD;

// Known attaching punctuation (including UTF-8 sequences).
// Static constexpr array — no heap allocation at startup unlike the old
// std::vector<std::string>.
struct PunctEntry {
  const char* str;
  uint8_t len;
};
static constexpr PunctEntry punctuation[] = {
    {".", 1},
    {",", 1},
    {"!", 1},
    {"?", 1},
    {";", 1},
    {":", 1},
    {"\"", 1},
    {"'", 1},
    {"\xE2\x80\x99", 3},  // ' (U+2019 right single quote)
    {"\xE2\x80\x9D", 3},  // " (U+201D right double quote)
};
static constexpr size_t PUNCT_COUNT = sizeof(punctuation) / sizeof(punctuation[0]);

// Check if a word consists entirely of attaching punctuation
// These should attach to the previous word without extra spacing
bool isAttachingPunctuationWord(const std::string& word) {
  if (word.empty()) return false;
  size_t pos = 0;
  while (pos < word.size()) {
    bool matched = false;
    for (size_t pi = 0; pi < PUNCT_COUNT; pi++) {
      if (word.compare(pos, punctuation[pi].len, punctuation[pi].str) == 0) {
        pos += punctuation[pi].len;
        matched = true;
        break;
      }
    }
    if (!matched) return false;
  }
  return true;
}

namespace {

bool containsSoftHyphen(const std::string& word) {
  for (size_t i = 0; i + 1 < word.size(); ++i) {
    if (static_cast<unsigned char>(word[i]) == SOFT_HYPHEN_BYTE1 &&
        static_cast<unsigned char>(word[i + 1]) == SOFT_HYPHEN_BYTE2) {
      return true;
    }
  }
  return false;
}

// Find all soft hyphen byte positions in a UTF-8 string
std::vector<size_t> findSoftHyphenPositions(const std::string& word) {
  std::vector<size_t> positions;
  for (size_t i = 0; i + 1 < word.size(); ++i) {
    if (static_cast<unsigned char>(word[i]) == SOFT_HYPHEN_BYTE1 &&
        static_cast<unsigned char>(word[i + 1]) == SOFT_HYPHEN_BYTE2) {
      positions.push_back(i);
    }
  }
  return positions;
}

// Remove all soft hyphens from a string
std::string stripSoftHyphens(const std::string& word) {
  std::string result;
  result.reserve(word.size());
  size_t i = 0;
  while (i < word.size()) {
    if (i + 1 < word.size() && static_cast<unsigned char>(word[i]) == SOFT_HYPHEN_BYTE1 &&
        static_cast<unsigned char>(word[i + 1]) == SOFT_HYPHEN_BYTE2) {
      i += 2;  // Skip soft hyphen
    } else {
      result += word[i++];
    }
  }
  return result;
}

// Check if word ends with a soft hyphen marker (U+00AD = 0xC2 0xAD)
bool hasTrailingSoftHyphen(const std::string& word) {
  return word.size() >= 2 && static_cast<unsigned char>(word[word.size() - 2]) == SOFT_HYPHEN_BYTE1 &&
         static_cast<unsigned char>(word[word.size() - 1]) == SOFT_HYPHEN_BYTE2;
}

bool isWordSeparatorCodepoint(uint32_t cp) {
  switch (cp) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case 0x00A0:  // NO-BREAK SPACE
    case 0x1680:  // OGHAM SPACE MARK
    case 0x2028:  // LINE SEPARATOR
    case 0x2029:  // PARAGRAPH SEPARATOR
    case 0x202F:  // NARROW NO-BREAK SPACE
    case 0x205F:  // MEDIUM MATHEMATICAL SPACE
    case 0x3000:  // IDEOGRAPHIC SPACE
    case 0xFEFF:  // ZERO WIDTH NO-BREAK SPACE / BOM
      return true;
    default:
      return cp >= 0x2000 && cp <= 0x200A;  // EN/EM/thin/hair/etc spaces
  }
}

// Replace trailing soft hyphen with visible ASCII hyphen for rendering
std::string replaceTrailingSoftHyphen(std::string word) {
  if (hasTrailingSoftHyphen(word)) {
    word.resize(word.size() - 2);
    word += '-';
  }
  return word;
}

// Get word prefix before soft hyphen position (stripped) + visible hyphen
std::string getWordPrefix(const std::string& word, size_t softHyphenPos) {
  std::string prefix = word.substr(0, softHyphenPos);
  return stripSoftHyphens(prefix) + "-";
}

// Get word suffix after soft hyphen position (keep soft hyphens for further splitting)
std::string getWordSuffix(const std::string& word, size_t softHyphenPos) {
  return word.substr(softHyphenPos + 2);  // Skip past soft hyphen bytes, DON'T strip
}

// Check if codepoint is CJK ideograph (Unicode Line Break Class ID)
// Based on UAX #14 - allows line break before/after these characters
bool isCjkCodepoint(uint32_t cp) {
  // CJK Unified Ideographs
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
  // CJK Extension A
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;
  // CJK Compatibility Ideographs
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;
  // Hiragana
  if (cp >= 0x3040 && cp <= 0x309F) return true;
  // Katakana
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;
  // Hangul Syllables
  if (cp >= 0xAC00 && cp <= 0xD7AF) return true;
  // CJK Extension B and beyond (Plane 2)
  if (cp >= 0x20000 && cp <= 0x2A6DF) return true;
  // Fullwidth ASCII variants (often used in CJK context)
  if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
  return false;
}

// Knuth-Plass: Calculate badness (looseness) of a line
// Returns cubic ratio penalty - loose lines are penalized more heavily
float calculateBadness(int lineWidth, int targetWidth) {
  if (targetWidth <= 0) return INFINITY_PENALTY;
  if (lineWidth > targetWidth) return INFINITY_PENALTY;
  if (lineWidth == targetWidth) return 0.0f;
  float ratio = static_cast<float>(targetWidth - lineWidth) / static_cast<float>(targetWidth);
  return ratio * ratio * ratio * 100.0f;
}

// Knuth-Plass: Calculate demerits for a line based on its badness
// Last line gets 0 demerits (allowed to be loose)
float calculateDemerits(float badness, bool isLastLine) {
  if (badness >= INFINITY_PENALTY) return INFINITY_PENALTY;
  if (isLastLine) return 0.0f;
  return (1.0f + badness) * (1.0f + badness);
}

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle) {
  if (word.empty()) return;

  // Some books contain NBSP or other Unicode whitespace inside what the parser
  // thinks is a single "word". Split those here so layout never treats them as
  // glued text.
  bool hasSeparator = false;
  bool hasCjk = false;
  const unsigned char* check = reinterpret_cast<const unsigned char*>(word.c_str());
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&check))) {
    if (isWordSeparatorCodepoint(cp)) {
      hasSeparator = true;
    } else if (isCjkCodepoint(cp)) {
      hasCjk = true;
    }
  }

  if (hasSeparator) {
    const unsigned char* segmentStart = reinterpret_cast<const unsigned char*>(word.c_str());
    const unsigned char* p = segmentStart;
    while (*p) {
      const unsigned char* cpStart = p;
      cp = utf8NextCodepoint(&p);
      if (!cp) break;
      if (isWordSeparatorCodepoint(cp)) {
        if (cpStart > segmentStart) {
          addWord(std::string(reinterpret_cast<const char*>(segmentStart), cpStart - segmentStart), fontStyle);
        }
        segmentStart = p;
      }
    }
    if (p > segmentStart) {
      addWord(std::string(reinterpret_cast<const char*>(segmentStart), p - segmentStart), fontStyle);
    }
    return;
  }

  if (!hasCjk) {
    // No CJK - keep as single word (Latin, accented Latin, Cyrillic, etc.)
    words.push_back({std::move(word), fontStyle});
    return;
  }

  // Mixed content: group non-CJK runs together, split CJK individually
  const unsigned char* p = reinterpret_cast<const unsigned char*>(word.c_str());
  std::string nonCjkBuf;

  while ((cp = utf8NextCodepoint(&p))) {
    if (isCjkCodepoint(cp)) {
      // CJK character - flush non-CJK buffer first, then add this char alone
      if (!nonCjkBuf.empty()) {
        words.push_back({std::move(nonCjkBuf), fontStyle});
        nonCjkBuf.clear();
      }

      // Re-encode CJK codepoint to UTF-8
      std::string buf;
      if (cp < 0x10000) {
        buf += static_cast<char>(0xE0 | (cp >> 12));
        buf += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        buf += static_cast<char>(0xF0 | (cp >> 18));
        buf += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        buf += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf += static_cast<char>(0x80 | (cp & 0x3F));
      }
      words.push_back({std::move(buf), fontStyle});
    } else {
      // Non-CJK character - accumulate into buffer
      if (cp < 0x80) {
        nonCjkBuf += static_cast<char>(cp);
      } else if (cp < 0x800) {
        nonCjkBuf += static_cast<char>(0xC0 | (cp >> 6));
        nonCjkBuf += static_cast<char>(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        nonCjkBuf += static_cast<char>(0xE0 | (cp >> 12));
        nonCjkBuf += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        nonCjkBuf += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        nonCjkBuf += static_cast<char>(0xF0 | (cp >> 18));
        nonCjkBuf += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        nonCjkBuf += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        nonCjkBuf += static_cast<char>(0x80 | (cp & 0x3F));
      }
    }
  }

  // Flush any remaining non-CJK buffer
  if (!nonCjkBuf.empty()) {
    words.push_back({std::move(nonCjkBuf), fontStyle});
  }
}

std::string ParsedText::previewText(size_t maxWords, size_t maxChars) const {
  std::string preview;
  size_t wordsUsed = 0;

  for (const auto& entry : words) {
    if (wordsUsed >= maxWords || preview.size() >= maxChars) {
      break;
    }

    std::string word = containsSoftHyphen(entry.word) ? stripSoftHyphens(entry.word) : entry.word;
    if (word.empty()) {
      continue;
    }

    const size_t separatorLen = preview.empty() ? 0U : 1U;
    if (preview.size() + separatorLen + word.size() > maxChars) {
      break;
    }

    if (!preview.empty()) {
      preview += ' ';
    }
    preview += word;
    ++wordsUsed;
  }

  return preview;
}

// Consumes data to minimize memory usage
// Returns false if aborted, true otherwise
bool ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine, const AbortCallback& shouldAbort) {
  if (words.empty()) {
    return true;
  }

  // Check for abort before starting
  if (shouldAbort && shouldAbort()) {
    return false;
  }

  const int pageWidth = viewportWidth;
  const int spaceWidth = renderer.getSpaceWidth(fontId);

  // Rejoin words that were split by a previous interrupted greedy layout pass.
  // Split prefixes are marked with trailing U+00AD; rejoin with the following suffix word.
  {
    size_t i = 0;
    while (i < words.size()) {
      if (i + 1 < words.size() && hasTrailingSoftHyphen(words[i].word)) {
        words[i].word.resize(words[i].word.size() - 2);  // Remove trailing U+00AD
        words[i].word += words[i + 1].word;               // Rejoin with suffix
        words.erase(words.begin() + static_cast<ptrdiff_t>(i + 1));
        // Don't advance - check if rejoined word also has marker (nested splits)
      } else {
        ++i;
      }
    }
  }

  // Pre-split oversized words at soft hyphen positions
  if (hyphenationEnabled) {
    if (!preSplitOversizedWords(renderer, fontId, pageWidth, shouldAbort)) {
      return false;  // Aborted
    }
  }

  auto wordWidths = calculateWordWidths(renderer, fontId);
  const auto lineBreakIndices =
      useGreedyBreaking ? computeLineBreaksGreedy(renderer, fontId, pageWidth, spaceWidth, wordWidths, shouldAbort)
                        : computeLineBreaks(pageWidth, spaceWidth, wordWidths, shouldAbort);

  // Check if we were aborted during line break computation
  if (shouldAbort && shouldAbort()) {
    return false;
  }

  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    if (shouldAbort && shouldAbort()) {
      return false;
    }
    extractLine(renderer, fontId, i, pageWidth, spaceWidth, wordWidths, lineBreakIndices, processLine);
  }
  return true;
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  const size_t totalWordCount = words.size();

  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(totalWordCount);

  // Add indentation at the beginning of first word in paragraph
  if (!indentApplied && indentLevel > 0 && !words.empty() && style != TextBlock::CENTER_ALIGN) {
    std::string& first_word = words.front().word;
    switch (indentLevel) {
      case 2:  // Normal - em-space (U+2003)
        first_word.insert(0, "\xe2\x80\x83");
        break;
      case 3:  // Large - em-space + en-space (U+2003 + U+2002)
        first_word.insert(0, "\xe2\x80\x83\xe2\x80\x82");
        break;
      default:  // Fallback for unexpected values: single en-space (U+2002)
        first_word.insert(0, "\xe2\x80\x82");
        break;
    }
    indentApplied = true;
  }

  for (auto& entry : words) {
    // Strip soft hyphens before measuring (they should be invisible)
    // After preSplitOversizedWords, words shouldn't contain soft hyphens,
    // but we strip here for safety and for when hyphenation is disabled
    if (containsSoftHyphen(entry.word)) {
      std::string displayWord = stripSoftHyphens(entry.word);
      wordWidths.push_back(renderer.getTextAdvanceWidth(fontId, displayWord.c_str(), entry.style));
      // Update the word with the stripped version for rendering
      entry.word = std::move(displayWord);
    } else {
      wordWidths.push_back(renderer.getTextAdvanceWidth(fontId, entry.word.c_str(), entry.style));
    }
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const int pageWidth, const int spaceWidth,
                                                  const std::vector<uint16_t>& wordWidths,
                                                  const AbortCallback& shouldAbort) const {
  const size_t n = words.size();

  if (n == 0) {
    return {};
  }

  // Forward DP: minDemerits[i] = minimum demerits to reach position i (before word i)
  std::vector<float> minDemerits(n + 1, INFINITY_PENALTY);
  std::vector<int> prevBreak(n + 1, -1);
  minDemerits[0] = 0.0f;

  for (size_t i = 0; i < n; i++) {
    // Check for abort periodically (every 100 words in outer loop)
    if (shouldAbort && (i % 100 == 0) && shouldAbort()) {
      return {};  // Return empty to signal abort
    }

    if (minDemerits[i] >= INFINITY_PENALTY) continue;

    int lineWidth = -spaceWidth;  // First word won't have preceding space
    for (size_t j = i; j < n; j++) {
      lineWidth += wordWidths[j] + spaceWidth;

      if (lineWidth > pageWidth) {
        if (j == i) {
          // Oversized word: force onto its own line with high penalty
          float demerits = 100.0f + LINE_PENALTY;
          if (minDemerits[i] + demerits < minDemerits[j + 1]) {
            minDemerits[j + 1] = minDemerits[i] + demerits;
            prevBreak[j + 1] = static_cast<int>(i);
          }
        }
        break;
      }

      bool isLastLine = (j == n - 1);
      float badness = calculateBadness(lineWidth, pageWidth);
      float demerits = calculateDemerits(badness, isLastLine) + LINE_PENALTY;

      if (minDemerits[i] + demerits < minDemerits[j + 1]) {
        minDemerits[j + 1] = minDemerits[i] + demerits;
        prevBreak[j + 1] = static_cast<int>(i);
      }
    }
  }

  // Backtrack to reconstruct line break indices
  std::vector<size_t> lineBreakIndices;
  int pos = static_cast<int>(n);
  while (pos > 0 && prevBreak[pos] >= 0) {
    lineBreakIndices.push_back(static_cast<size_t>(pos));
    pos = prevBreak[pos];
  }
  std::reverse(lineBreakIndices.begin(), lineBreakIndices.end());

  // Fallback: if backtracking failed or chain is incomplete, use single-word-per-line
  // After the loop, pos should be 0 if we successfully traced back to the start.
  // If pos > 0, the chain is incomplete (no valid path from position 0 to n).
  if (lineBreakIndices.empty() || pos != 0) {
    lineBreakIndices.clear();
    for (size_t i = 1; i <= n; i++) {
      lineBreakIndices.push_back(i);
    }
  }

  return lineBreakIndices;
}

std::vector<size_t> ParsedText::computeLineBreaksGreedy(const GfxRenderer& renderer, const int fontId,
                                                        const int pageWidth, const int spaceWidth,
                                                        std::vector<uint16_t>& wordWidths,
                                                        const AbortCallback& shouldAbort) {
  std::vector<size_t> breaks;
  size_t n = wordWidths.size();

  if (n == 0) {
    return breaks;
  }

  int lineWidth = -spaceWidth;  // First word won't have preceding space
  size_t wordsOnCurrentLine = 0;
  for (size_t i = 0; i < n; i++) {
    // Check for abort periodically (every 200 words)
    if (shouldAbort && (i % 200 == 0) && shouldAbort()) {
      return {};  // Return empty to signal abort
    }

    const int wordWidth = wordWidths[i];
    if (wordsOnCurrentLine >= MAX_GREEDY_WORDS_PER_LINE && lineWidth > 0) {
      LOG_ERR(TAG, "Greedy line clamp triggered: words=%u pageWidth=%d", static_cast<unsigned>(wordsOnCurrentLine),
              pageWidth);
      breaks.push_back(i);
      lineWidth = wordWidth;
      wordsOnCurrentLine = 1;
      continue;
    }

    // Check if adding this word would overflow the line
    if (lineWidth + wordWidth + spaceWidth > pageWidth && lineWidth > 0) {
      // Try to hyphenate: split the overflowing word so its first part fits on this line
      const int remainingWidth = pageWidth - lineWidth - spaceWidth;
      if (remainingWidth > 0 && trySplitWordForLineEnd(renderer, fontId, remainingWidth, i, wordWidths)) {
        // Word was split: prefix is at index i (fits on current line), suffix at i+1
        lineWidth += wordWidths[i] + spaceWidth;
        n = wordWidths.size();  // Vector grew by one
        // End this line after the prefix
        breaks.push_back(i + 1);
        // Next iteration (i+1) starts the suffix on a new line
        lineWidth = -spaceWidth;  // Will be updated when we process i+1
        wordsOnCurrentLine = 0;
      } else {
        // No hyphenation possible - start a new line at this word
        breaks.push_back(i);
        lineWidth = wordWidth;
        wordsOnCurrentLine = 1;
      }
    } else {
      lineWidth += wordWidth + spaceWidth;
      wordsOnCurrentLine++;
    }
  }

  // Final break at end of all words
  breaks.push_back(n);
  return breaks;
}

void ParsedText::extractLine(const GfxRenderer& renderer, const int fontId, const size_t breakIndex,
                             const int pageWidth, const int spaceWidth,
                             const std::vector<uint16_t>& wordWidths, const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine) {
  (void)spaceWidth;
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  // Calculate total word width for this line and count actual word gaps
  // (punctuation that attaches to previous word doesn't count as a gap)
  int lineWordWidthSum = 0;
  int naturalGapWidthSum = 0;
  size_t actualGapCount = 0;
  EpdFontFamily::Style previousStyle = EpdFontFamily::REGULAR;
  gapWidthsPool_.resize(lineWordCount);
  std::fill(gapWidthsPool_.begin(), gapWidthsPool_.end(), 0);
  auto& gapWidths = gapWidthsPool_;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    if (wordIdx > 0) {
      if (!isAttachingPunctuationWord(words[wordIdx].word)) {
        const uint16_t gapWidth =
            static_cast<uint16_t>(std::max(renderer.getSpaceWidth(fontId, previousStyle),
                                           renderer.getSpaceWidth(fontId, words[wordIdx].style)));
        gapWidths[wordIdx] = gapWidth;
        naturalGapWidthSum += gapWidth;
        actualGapCount++;
      }
    }
    previousStyle = words[wordIdx].style;
  }

  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;
  const int contentWidth = lineWordWidthSum + naturalGapWidthSum;
  const int spareSpace = std::max(0, pageWidth - contentWidth);
  const bool isJustifiedLine = style == TextBlock::JUSTIFIED && !isLastLine && actualGapCount >= 1;
  const int justifyExtraPerGap = isJustifiedLine ? spareSpace / static_cast<int>(actualGapCount) : 0;
  int justifyExtraRemainder = isJustifiedLine ? spareSpace % static_cast<int>(actualGapCount) : 0;

  // For RTL text, default to right alignment
  const auto effectiveStyle = (isRtl && style == TextBlock::LEFT_ALIGN) ? TextBlock::RIGHT_ALIGN : style;

  // Build WordData vector directly, consuming from front of lists
  // Punctuation that attaches to the previous word doesn't get space before it
  std::vector<TextBlock::WordInput> lineData;
  lineData.reserve(lineWordCount);

  if (isRtl) {
    // RTL: Position words from right to left
    int xpos;
    if (effectiveStyle == TextBlock::CENTER_ALIGN) {
      xpos = pageWidth - (spareSpace / 2);
    } else {
      xpos = pageWidth;  // RIGHT_ALIGN and JUSTIFIED start from right edge
    }

    for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
      const uint16_t currentWordWidth = wordWidths[lastBreakAt + wordIdx];
      xpos -= currentWordWidth;
      lineData.push_back({replaceTrailingSoftHyphen(std::move(words[wordIdx].word)),
                          static_cast<uint16_t>(std::max(0, xpos)), words[wordIdx].style});

      if (wordIdx + 1 < lineWordCount) {
        int gapWidth = gapWidths[wordIdx + 1];
        if (isJustifiedLine && gapWidth > 0) {
          gapWidth += justifyExtraPerGap;
          if (justifyExtraRemainder > 0) {
            gapWidth++;
            justifyExtraRemainder--;
          }
        }
        xpos -= gapWidth;
      }
    }
  } else {
    // LTR: Position words from left to right
    int xpos = 0;
    if (effectiveStyle == TextBlock::RIGHT_ALIGN) {
      xpos = spareSpace;
    } else if (effectiveStyle == TextBlock::CENTER_ALIGN) {
      xpos = spareSpace / 2;
    }

    for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
      const uint16_t currentWordWidth = wordWidths[lastBreakAt + wordIdx];
      lineData.push_back({replaceTrailingSoftHyphen(std::move(words[wordIdx].word)),
                          static_cast<uint16_t>(std::max(0, xpos)), words[wordIdx].style});

      xpos += currentWordWidth;
      if (wordIdx + 1 < lineWordCount) {
        int gapWidth = gapWidths[wordIdx + 1];
        if (isJustifiedLine && gapWidth > 0) {
          gapWidth += justifyExtraPerGap;
          if (justifyExtraRemainder > 0) {
            gapWidth++;
            justifyExtraRemainder--;
          }
        }
        xpos += gapWidth;
      }
    }
  }

  // Remove consumed elements from the front
  words.erase(words.begin(), words.begin() + static_cast<ptrdiff_t>(lineWordCount));

  processLine(TextBlock::fromWords(lineData, effectiveStyle));
}

bool ParsedText::preSplitOversizedWords(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                        const AbortCallback& shouldAbort) {
  std::vector<WordEntry> pendingWords;
  std::vector<WordEntry> newWords;
  pendingWords.swap(words);
  newWords.reserve(pendingWords.size());  // At least as many words as input

  size_t readIdx = 0;

  while (readIdx < pendingWords.size()) {
    // Check for abort periodically (every 50 words)
    if (shouldAbort && (readIdx % 50 == 0) && readIdx > 0 && shouldAbort()) {
      // Move remaining words to output
      for (size_t j = readIdx; j < pendingWords.size(); j++) {
        newWords.push_back(std::move(pendingWords[j]));
      }
      words.swap(newWords);
      return false;  // Aborted
    }

    WordEntry current = std::move(pendingWords[readIdx++]);

    std::string word = std::move(current.word);
    const EpdFontFamily::Style wordStyle = current.style;

    // Measure word without soft hyphens
    const bool hasSoftHyphen = containsSoftHyphen(word);
    const std::string stripped = hasSoftHyphen ? stripSoftHyphens(word) : word;
    const int wordWidth = renderer.getTextAdvanceWidth(fontId, stripped.c_str(), wordStyle);

    if (wordWidth <= pageWidth) {
      // Word fits, keep as-is (will be stripped later in calculateWordWidths)
      newWords.push_back({word, wordStyle});
    } else {
      // Word is too wide - try to split at soft hyphen positions
      auto shyPositions = findSoftHyphenPositions(word);

      if (shyPositions.empty()) {
        // No soft hyphens - use dictionary-based hyphenation
        // Compute all break points on the full word once (Liang patterns
        // need full-word context for correct results).
        auto breaks = Hyphenation::breakOffsets(word, true);

        if (breaks.empty()) {
          newWords.push_back({word, wordStyle});
        } else {
          size_t prevOffset = 0;

          for (size_t bi = 0; bi <= breaks.size(); ++bi) {
            const std::string remaining = word.substr(prevOffset);
            const int remainingWidth = renderer.getTextAdvanceWidth(fontId, remaining.c_str(), wordStyle);

            if (remainingWidth <= pageWidth) {
              newWords.push_back({remaining, wordStyle});
              break;
            }

            // Find the rightmost break where prefix + hyphen fits
            int bestIdx = -1;
            std::string bestPrefix;
            for (int i = static_cast<int>(breaks.size()) - 1; i >= 0; --i) {
              if (breaks[i].byteOffset <= prevOffset) continue;
              std::string prefix = word.substr(prevOffset, breaks[i].byteOffset - prevOffset);
              if (breaks[i].requiresInsertedHyphen) {
                prefix += "-";
              }
              const int prefixWidth = renderer.getTextAdvanceWidth(fontId, prefix.c_str(), wordStyle);
              if (prefixWidth <= pageWidth) {
                bestIdx = i;
                bestPrefix = std::move(prefix);
                break;
              }
            }

            if (bestIdx < 0) {
              newWords.push_back({remaining, wordStyle});
              break;
            }

            newWords.push_back({std::move(bestPrefix), wordStyle});
            prevOffset = breaks[bestIdx].byteOffset;
          }
        }
      } else {
        // Split word at soft hyphen positions
        std::string remaining = word;
        size_t splitIterations = 0;
        constexpr size_t MAX_SPLIT_ITERATIONS = 100;  // Safety limit

        while (splitIterations++ < MAX_SPLIT_ITERATIONS) {
          if (splitIterations == MAX_SPLIT_ITERATIONS) {
            LOG_ERR(TAG, "Warning: hit max split iterations for oversized word");
          }
          const std::string strippedRemaining = containsSoftHyphen(remaining) ? stripSoftHyphens(remaining) : remaining;
          const int remainingWidth = renderer.getTextAdvanceWidth(fontId, strippedRemaining.c_str(), wordStyle);

          if (remainingWidth <= pageWidth) {
            // Remaining part fits, add it and done
            newWords.push_back({remaining, wordStyle});
            break;
          }

          // Find soft hyphen positions in remaining string
          auto localPositions = findSoftHyphenPositions(remaining);
          if (localPositions.empty()) {
            // No more soft hyphens, output as-is
            newWords.push_back({remaining, wordStyle});
            break;
          }

          // Find the rightmost soft hyphen where prefix + hyphen fits
          int bestPos = -1;
          for (int i = static_cast<int>(localPositions.size()) - 1; i >= 0; --i) {
            std::string prefix = getWordPrefix(remaining, localPositions[i]);
            int prefixWidth = renderer.getTextAdvanceWidth(fontId, prefix.c_str(), wordStyle);
            if (prefixWidth <= pageWidth) {
              bestPos = i;
              break;
            }
          }

          if (bestPos < 0) {
            // Even the smallest prefix is too wide - output as-is
            newWords.push_back({remaining, wordStyle});
            break;
          }

          // Split at this position
          std::string prefix = getWordPrefix(remaining, localPositions[bestPos]);
          std::string suffix = getWordSuffix(remaining, localPositions[bestPos]);

          newWords.push_back({std::move(prefix), wordStyle});  // Already includes visible hyphen "-"

          if (suffix.empty()) {
            break;
          }
          remaining = suffix;
        }
      }
    }

  }

  words.swap(newWords);
  return true;
}

bool ParsedText::trySplitWordForLineEnd(const GfxRenderer& renderer, const int fontId, const int remainingWidth,
                                        const size_t wordIndex, std::vector<uint16_t>& wordWidths) {
  if (!hyphenationEnabled) return false;

  const std::string& word = words[wordIndex].word;
  const EpdFontFamily::Style fontStyle = words[wordIndex].style;

  auto breaks = Hyphenation::breakOffsets(word, false);
  if (breaks.empty()) return false;

  // Find rightmost break where prefix+hyphen fits in remainingWidth
  for (int i = static_cast<int>(breaks.size()) - 1; i >= 0; --i) {
    std::string prefix = word.substr(0, breaks[i].byteOffset);
    // Measure with visible hyphen for accurate layout
    const std::string displayPrefix = breaks[i].requiresInsertedHyphen ? prefix + "-" : prefix;
    const int prefixWidth = renderer.getTextAdvanceWidth(fontId, displayPrefix.c_str(), fontStyle);
    if (prefixWidth <= remainingWidth) {
      // Store with soft hyphen MARKER (not visible hyphen) so interrupted layouts
      // can rejoin the fragments on resume (calculateWordWidths strips U+00AD)
      if (breaks[i].requiresInsertedHyphen) prefix += "\xC2\xAD";
      std::string suffix = word.substr(breaks[i].byteOffset);
      const int suffixWidth = renderer.getTextAdvanceWidth(fontId, suffix.c_str(), fontStyle);

      // Replace current word with prefix, insert suffix after
      words[wordIndex].word = std::move(prefix);
      words.insert(words.begin() + static_cast<ptrdiff_t>(wordIndex + 1),
                   WordEntry{std::move(suffix), fontStyle});

      // Update widths vector
      wordWidths[wordIndex] = static_cast<uint16_t>(prefixWidth);
      wordWidths.insert(wordWidths.begin() + wordIndex + 1, static_cast<uint16_t>(suffixWidth));
      return true;
    }
  }
  return false;
}
