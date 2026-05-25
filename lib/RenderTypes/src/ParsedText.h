#pragma once

#include <EpdFontFamily.h>
#include <LittleFS.h>  // for fs::File spillAppendFile_

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/TextBlock.h"

class GfxRenderer;

/**
 * Callback type for checking if operation should abort.
 * Returns true if caller should stop work and return early.
 */
using AbortCallback = std::function<bool()>;

class ParsedText {
  struct WordEntry {
    std::string word;
    EpdFontFamily::Style style;
  };

  // v2.0.107 (Phase 4c — heap-fragmentation fix):
  //
  // Was `std::vector<WordEntry>` with `reserve(64) = 1792 B` contiguous
  // allocation per text block.  The audit identified this as the
  // proximate trigger for the `largest_free=7668` collapse seen on
  // heavy Calibre chapters: every paragraph paid a single ~1.8 KB
  // contiguous heap allocation that landed in the largest free hole,
  // shrinking the next-largest below `MIN_LAYOUT_FREE_HEAP` and
  // tripping the layout abort.
  //
  // std::deque allocates in fixed-size chunks (~224 B for WordEntry
  // sized elements on this platform).  A 50-word paragraph now uses
  // ~7 small allocations instead of one big contiguous block.  Same
  // total heap, but many small fragments are MUCH friendlier to the
  // allocator: large free holes stay intact, the layout watermark
  // (which only cares about largest-free) stays high.
  //
  // API note: deque preserves references across end-insertions (push_back,
  // push_front) — different from vector which invalidates on resize.
  // All existing call sites that take references to elements (e.g.
  // `std::string& first_word = words.front().word;` in
  // calculateWordWidths) remain correct.  Random access via
  // `words[i]` is O(1) just like vector.  Erasure / insertion at
  // non-end positions is O(N) on both containers; we do that rarely
  // (soft-hyphen rejoin, oversized-word presplit) so the cost is in
  // the noise.
  std::deque<WordEntry> words;
  TextBlock::BLOCK_STYLE style;
  uint8_t indentLevel;
  bool hyphenationEnabled;
  bool useGreedyBreaking = true;  // Default to greedy to avoid Knuth-Plass memory spike
  bool isRtl = false;
  bool indentApplied = false;

  // Pooled buffer reused across extractLine calls to avoid per-line heap allocation
  std::vector<uint16_t> gapWidthsPool_;
  std::string spillPath_;
  uint32_t spillReadOffset_ = 0;
  uint16_t spillWords_ = 0;
  bool spillWriteFailed_ = false;
  // v2.0.110 (ChatGPT-audit perf fix): persistent append handle so we don't
  // pay LittleFS file-open overhead (~5 ms per call) once per spilled word.
  // Pre-fix: 96 words per spill × 5 ms = 480 ms wasted opening/closing the
  // same file 96 times.  With a persistent handle, those 96 calls cost
  // ~0.1 ms each (just write + filesystem-level seek).  Handle is closed
  // before layoutSpilledGreedy reopens for read, and in cleanupSpill so
  // destruction is safe.  Forward-declared `fs::File` requires LittleFS.h
  // include in the .cpp where this member is touched.
  File spillAppendFile_;

  std::vector<size_t> computeLineBreaks(int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths,
                                        const AbortCallback& shouldAbort = nullptr) const;
  std::vector<size_t> computeLineBreaksGreedy(const GfxRenderer& renderer, int fontId, int pageWidth, int spaceWidth,
                                              std::vector<uint16_t>& wordWidths,
                                              const AbortCallback& shouldAbort = nullptr);
  bool trySplitWordForLineEnd(const GfxRenderer& renderer, int fontId, int remainingWidth,
                              size_t wordIndex, std::vector<uint16_t>& wordWidths);
  void extractLine(const GfxRenderer& renderer, int fontId, size_t breakIndex, int pageWidth, int spaceWidth,
                   const std::vector<uint16_t>& wordWidths,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);
  bool preSplitOversizedWords(const GfxRenderer& renderer, int fontId, int pageWidth,
                              const AbortCallback& shouldAbort = nullptr);
  bool storeWord(std::string word, EpdFontFamily::Style fontStyle);
  bool appendSpillWord(const std::string& word, EpdFontFamily::Style fontStyle);
  bool readSpillWord(File& file, WordEntry& out);
  bool layoutSpilledGreedy(const GfxRenderer& renderer, int fontId, int pageWidth, int spaceWidth,
                           const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                           bool includeLastLine = true, const AbortCallback& shouldAbort = nullptr);
  void cleanupSpill();

 public:
  explicit ParsedText(const TextBlock::BLOCK_STYLE style, const uint8_t indentLevel,
                      const bool hyphenationEnabled = true, const bool useGreedy = true, const bool rtl = false)
      : style(style),
        indentLevel(indentLevel),
        hyphenationEnabled(hyphenationEnabled),
        useGreedyBreaking(useGreedy),
        isRtl(rtl) {
    // v2.0.107: std::deque needs no pre-reserve — its chunked growth
    // adds blocks of ~224 B as needed, no single large contiguous
    // allocation that could trip on a fragmented heap.  The v2.0.64
    // `reserve(64) = 1792 B contiguous` allocation it replaces was
    // the dominant per-text-block heap fragmenter.
  }
  ~ParsedText();

  void addWord(std::string word, EpdFontFamily::Style fontStyle);
  void setStyle(const TextBlock::BLOCK_STYLE style) { this->style = style; }
  void setRtl(const bool rtl) { isRtl = rtl; }
  void setUseGreedyBreaking(const bool greedy) { useGreedyBreaking = greedy; }
  TextBlock::BLOCK_STYLE getStyle() const { return style; }
  size_t size() const { return words.size() + spillWords_; }
  bool isEmpty() const { return words.empty() && spillWords_ == 0; }
  std::string previewText(size_t maxWords = 8, size_t maxChars = 96) const;
  bool layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true, const AbortCallback& shouldAbort = nullptr);
};
