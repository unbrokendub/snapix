#pragma once

#include <ParsedText.h>
#include <RenderConfig.h>
#include <blocks/ImageBlock.h>
#include <blocks/TextBlock.h>
#include <expat.h>

#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../css/CssParser.h"
#include "DataUriStripper.h"

class Page;
class GfxRenderer;
class Print;

#define MAX_WORD_SIZE 200
constexpr int MAX_XML_DEPTH = 100;

class ChapterHtmlSlimParser {
 public:
  enum class ReadItemStatus : uint8_t {
    Success,
    Aborted,
    NotFound,
    ArchiveError,
    IoError,
    WriteError,
  };

  enum class CachedImageStatus : uint8_t {
    Success,
    RetryableInterruption,
    TerminalFailure,
  };

  enum class ImageFailureClass : uint8_t {
    None,
    AbortRequested,
    Timeout,
    LowMemory,
    DataUri,
    UnsupportedFormat,
    CacheHit,
    CachedOpenFailed,
    CachedBmpInvalid,
    TempOpenFailed,
    ExtractAborted,
    ExtractFailed,
    ConvertAborted,
    ConvertFailed,
    GeneratedBmpInvalid,
    MissingSrc,
    ReadItemUnavailable,
    ImageCacheDisabled,
  };

 private:
  const std::string filepath;
  GfxRenderer& renderer;
  std::function<bool(std::unique_ptr<Page>)> completePageFn;  // Returns false to stop parsing
  std::function<void(int)> progressFn;                        // Progress callback (0-100)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int cssBoldUntilDepth = INT_MAX;
  int cssItalicUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  RenderConfig config;
  int effectiveLineHeight_ = 0;

  // Image support
  std::string chapterBasePath;
  std::string imageCachePath;
  std::function<ReadItemStatus(const std::string&, Print&, size_t, const std::function<bool()>&)> readItemFn;
  bool quickImageDecode_ = false;
  std::shared_ptr<ImageBlock> pendingPageImage_ = nullptr;
  struct CachedImageResult {
    std::string cachedBmpPath;
    std::string resolvedPath;
    std::string failureReason;
    uint16_t width = 0;
    uint16_t height = 0;
    bool cacheHit = false;
    bool success = false;
    bool retryable = false;
    CachedImageStatus status = CachedImageStatus::TerminalFailure;
    ImageFailureClass failureClass = ImageFailureClass::None;
  };

  // CSS support
  const CssParser* cssParser_ = nullptr;

  // XML parser handle for stopping mid-parse
  XML_Parser xmlParser_ = nullptr;
  bool stopRequested_ = false;
  bool pendingEmergencySplit_ = false;
  bool pendingNewTextBlock_ = false;
  bool pendingBlockLikelyCaption_ = false;
  bool currentBlockLikelyCaption_ = false;
  TextBlock::BLOCK_STYLE pendingBlockStyle_ = TextBlock::LEFT_ALIGN;
  bool pendingRtl_ = false;
  int rtlUntilDepth_ = INT_MAX;

  // CSS text-align inheritance stack (text-align is an inherited property in CSS)
  struct AlignEntry {
    int depth;
    TextBlock::BLOCK_STYLE style;
  };
  std::vector<AlignEntry> alignStack_;

  struct ListEntry {
    int depth;
    bool isOrdered;
    int counter;
  };
  std::vector<ListEntry> listStack_;
  char pendingListMarker_[12] = {};

  bool aborted_ = false;
  bool cooperativeAbortRequested_ = false;
  bool fatalAbortRequested_ = false;

  // External abort callback for cooperative cancellation
  std::function<bool()> externalAbortCallback_ = nullptr;

  // Image failure rate limiting - skip remaining images after consecutive failures
  uint8_t consecutiveImageFailures_ = 0;
  static constexpr uint8_t MAX_CONSECUTIVE_IMAGE_FAILURES = 3;
  static constexpr uint32_t MAX_QUICK_IMAGE_PROCESS_TIME_MS = 8000;
  static constexpr uint32_t MAX_FULL_IMAGE_PROCESS_TIME_MS = 12000;

  // Session-wide blacklist of images that timed out or aborted during conversion.
  // Prevents background cold-extend from repeatedly grinding the same problematic
  // image (e.g. a large JPEG that takes >8s to convert).  Reset on reboot.
  static std::unordered_set<size_t>& sessionFailedImageHashes() {
    static std::unordered_set<size_t> instance;
    return instance;
  }

  // Parser safety - timeout and memory checks
  uint32_t parseStartTime_ = 0;
  uint16_t loopCounter_ = 0;
  uint16_t pagesCreated_ = 0;
  uint16_t elementCounter_ = 0;
  bool cssHeapOk_ = true;
  static constexpr uint32_t MAX_PARSE_TIME_MS = 20000;     // 20 second timeout
  static constexpr uint16_t YIELD_CHECK_INTERVAL = 100;    // Check every 100 iterations
  static constexpr uint16_t CSS_HEAP_CHECK_INTERVAL = 64;  // Check heap for CSS every 64 elements
  static constexpr size_t MIN_FREE_HEAP = 8192;            // 8KB minimum free heap
  // During JPEG conversion the decoder's own buffers (~6-7KB) consume part of
  // the largest free block, making the generic 8KB threshold fire on its own
  // allocations.  4KB is enough headroom for SPI I/O and FreeRTOS interrupts.
  static constexpr size_t MIN_IMAGE_PROCESS_FREE_HEAP = 4096;
  // Layout needs a bit more headroom than the generic abort threshold, but the
  // old 2x multiplier was too conservative on fragmented heaps and could block
  // progress forever even when layout still had enough room to finish.
  static constexpr size_t MIN_LAYOUT_FREE_HEAP = MIN_FREE_HEAP + 2048;

  // Pre-parse data URI stripper to prevent expat OOM on large embedded images
  DataUriStripper dataUriStripper_;

  // Anchor-to-page mapping: element id → page index (0-based)
  std::vector<std::pair<std::string, uint16_t>> anchorMap_;

  // Check if parsing should abort due to timeout or memory pressure
  bool shouldAbort();
  void requestCooperativeSuspend();

  static bool asciiContainsInsensitive(const char* haystack, const char* needle);
  static bool looksLikeCaptionTag(const char* name, const char* classAttr, const char* idAttr);
  bool looksLikeCaptionText(const ParsedText& text) const;
  bool measureTextBlockLineCount(const ParsedText& text, size_t& lineCount);
  int paragraphSpacing() const;
  void recomputeCurrentPageNextY();
  bool completeCurrentPageAndStartFresh();
  void maybeKeepTrailingImageWithCaption(bool likelyCaption);
  void maybeDeferTrailingTallImageText(bool likelyCaption);
  void startNewTextBlock(TextBlock::BLOCK_STYLE style, bool likelyCaption = false);
  void appendPartWordBytes(const char* data, int len);
  void flushPartWordBuffer();
  void makePages();
  static bool validateCachedBmp(const std::string& cachedBmpPath, uint16_t& width, uint16_t& height,
                                std::string& failureReason);
  CachedImageResult cacheImage(const std::string& src);
  void addImageToPage(std::shared_ptr<ImageBlock> image);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL defaultHandler(void* userData, const XML_Char* s, int len);

  // Suspend/resume state
  FsFile file_;
  size_t totalSize_ = 0;
  size_t bytesRead_ = 0;
  int lastProgress_ = -1;
  bool suspended_ = false;  // True when parser is suspended mid-parse (can resume)
  bool xmlDone_ = false;    // XML parser finished but content remains to flush

  bool initParser();
  bool parseLoop();
  void cleanupParser();
  bool placePendingImage();

 public:
  explicit ChapterHtmlSlimParser(const std::string& filepath, GfxRenderer& renderer, const RenderConfig& config,
                                 const std::function<bool(std::unique_ptr<Page>)>& completePageFn,
                                 const std::function<void(int)>& progressFn = nullptr,
                                 const std::string& chapterBasePath = "", const std::string& imageCachePath = "",
                                 const std::function<ReadItemStatus(const std::string&, Print&, size_t,
                                                                    const std::function<bool()>&)>& readItemFn =
                                     nullptr,
                                 bool quickImageDecode = false,
                                 const CssParser* cssParser = nullptr,
                                 const std::function<bool()>& externalAbortCallback = nullptr)
      : filepath(filepath),
        renderer(renderer),
        config(config),
        completePageFn(completePageFn),
        progressFn(progressFn),
        chapterBasePath(chapterBasePath),
        imageCachePath(imageCachePath),
        readItemFn(readItemFn),
        quickImageDecode_(quickImageDecode),
        cssParser_(cssParser),
        externalAbortCallback_(externalAbortCallback) {}
  ~ChapterHtmlSlimParser();
  bool parseAndBuildPages();
  bool resumeParsing();
  bool isSuspended() const { return suspended_; }
  void addLineToPage(std::shared_ptr<TextBlock> line);
  bool wasAborted() const { return aborted_; }
  const std::vector<std::pair<std::string, uint16_t>>& getAnchorMap() const { return anchorMap_; }
};
