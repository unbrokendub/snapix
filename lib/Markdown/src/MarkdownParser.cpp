/**
 * MarkdownParser.cpp
 *
 * Markdown parser implementation using md_parser tokenization.
 * Reads directly from SD card with minimal memory usage.
 */

#include "MarkdownParser.h"

#include <EpdFontFamily.h>
#include <FS.h>          // v2.0.192 — Arduino File for LittleFS reads
#include <LittleFS.h>    // v2.0.192 — cp1251 cache lives on LittleFS
#include <GfxRenderer.h>
#include <Logging.h>
#include <Page.h>
#include <ParsedText.h>
#include <SDCardManager.h>
#include <Utf8.h>
#include <blocks/TextBlock.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "md_parser.h"

#define TAG "MD_PARSE"

namespace {
bool isWhitespaceChar(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// v2.0.192 — same tagged-union RAII pattern as PlainTextParser's
// AnyFile (v2.0.189), extended with fgets() support.  FsFile (SdFat)
// has native fgets; Arduino File (FS.h) doesn't, so we emulate it
// for the LittleFS path via a small internal read buffer.
//
// The buffer (kBufSize bytes) is refilled from `lfsFile_.read()` when
// drained.  `position()` returns the LOGICAL cursor (bufStart + bufPos)
// which is what the caller expects (SdFat's fgets would advance the
// file position by exactly the bytes returned).  seek() invalidates
// the buffer.
class AnyFile {
 public:
  AnyFile() = default;
  AnyFile(const AnyFile&) = delete;
  AnyFile& operator=(const AnyFile&) = delete;
  ~AnyFile() { close(); }

  bool openSd(const std::string& path) {
    if (!SdMan.openFileForRead("MD ", path, sdFile_)) return false;
    useLittleFs_ = false;
    open_ = true;
    return true;
  }
  bool openLittleFs(const std::string& path) {
    lfsFile_ = LittleFS.open(path.c_str(), "r");
    if (!lfsFile_) return false;
    useLittleFs_ = true;
    open_ = true;
    bufStart_ = 0;
    bufLen_ = 0;
    bufPos_ = 0;
    return true;
  }

  size_t size() { return useLittleFs_ ? lfsFile_.size() : sdFile_.size(); }
  size_t position() {
    if (useLittleFs_) {
      // Logical position = where the buffer starts in the file + how
      // many buffered bytes the caller has consumed.
      return bufStart_ + bufPos_;
    }
    return sdFile_.position();
  }
  bool seek(size_t pos) {
    if (useLittleFs_) {
      invalidateBuffer();
      return lfsFile_.seek(pos);
    }
    return sdFile_.seek(pos);
  }
  bool seekSet(size_t pos) {
    if (useLittleFs_) {
      invalidateBuffer();
      return lfsFile_.seek(pos, SeekSet);
    }
    return sdFile_.seekSet(pos);
  }
  int read(uint8_t* buf, size_t len) {
    if (useLittleFs_) {
      // For raw reads, drain any buffered bytes first to keep `position()`
      // consistent with what the caller would expect.  In practice
      // MarkdownParser uses fgets exclusively (read() unused) so this
      // path is just defensive.
      invalidateBuffer();
      return lfsFile_.read(buf, len);
    }
    return sdFile_.read(buf, len);
  }
  void close() {
    if (!open_) return;
    if (useLittleFs_) {
      lfsFile_.close();
    } else {
      sdFile_.close();
    }
    open_ = false;
  }

  // Read one line up to (max-1) chars or until '\n' (inclusive).
  // Null-terminates.  Returns number of chars written (NOT including
  // the null).  Returns 0 on EOF.  Mirrors SdFat's fgets semantics:
  // the trailing '\n' (if present) is included in the returned count.
  int fgets(char* out, int max) {
    if (max <= 0) return 0;
    if (useLittleFs_) {
      return fgetsLittleFs(out, max);
    }
    return sdFile_.fgets(out, max);
  }

 private:
  static constexpr size_t kBufSize = 512;
  FsFile sdFile_;
  File lfsFile_;
  uint8_t buf_[kBufSize];
  size_t bufStart_ = 0;  // file offset where buf_[0] came from
  size_t bufLen_ = 0;    // how many bytes are in buf_
  size_t bufPos_ = 0;    // next byte to serve from buf_
  bool useLittleFs_ = false;
  bool open_ = false;

  void invalidateBuffer() {
    bufStart_ = useLittleFs_ ? lfsFile_.position() : 0;
    bufLen_ = 0;
    bufPos_ = 0;
  }

  bool refillBuffer() {
    bufStart_ = lfsFile_.position();
    const int n = lfsFile_.read(buf_, kBufSize);
    if (n <= 0) {
      bufLen_ = 0;
      bufPos_ = 0;
      return false;
    }
    bufLen_ = static_cast<size_t>(n);
    bufPos_ = 0;
    return true;
  }

  int fgetsLittleFs(char* out, int max) {
    int outPos = 0;
    while (outPos < max - 1) {
      if (bufPos_ >= bufLen_) {
        if (!refillBuffer()) break;  // EOF
      }
      const char c = static_cast<char>(buf_[bufPos_++]);
      out[outPos++] = c;
      if (c == '\n') break;
    }
    out[outPos] = '\0';
    return outPos;
  }
};
}  // namespace

namespace {

int utf8SafePrefixLength(const char* data, const int len, const int maxBytes) {
  const int limit = std::min(len, maxBytes);
  int consumed = 0;

  while (consumed < limit) {
    const unsigned char lead = static_cast<unsigned char>(data[consumed]);
    int cpLen = 1;
    if ((lead & 0x80U) == 0) {
      cpLen = 1;
    } else if ((lead & 0xE0U) == 0xC0U) {
      cpLen = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
      cpLen = 3;
    } else if ((lead & 0xF8U) == 0xF0U) {
      cpLen = 4;
    }

    if (consumed + cpLen > limit) {
      break;
    }
    consumed += cpLen;
  }

  return consumed > 0 ? consumed : limit;
}
}  // namespace

MarkdownParser::MarkdownParser(std::string filepath, GfxRenderer& renderer, const RenderConfig& config,
                               bool useLittleFs)
    : filepath_(std::move(filepath)), renderer_(renderer), config_(config), useLittleFs_(useLittleFs) {
  lineBuffer_[0] = '\0';
}

MarkdownParser::~MarkdownParser() = default;

void MarkdownParser::reset() {
  currentOffset_ = 0;
  hasMore_ = true;
  isRtl_ = false;
  pendingTextBlock_.reset();
  pendingPage_.reset();
  pendingPageNextY_ = 0;
}

int MarkdownParser::getCurrentFontStyle(const ParseContext& ctx) const {
  if (ctx.inBold && ctx.inItalic) {
    return EpdFontFamily::BOLD_ITALIC;
  } else if (ctx.inBold) {
    return EpdFontFamily::BOLD;
  } else if (ctx.inItalic) {
    return EpdFontFamily::ITALIC;
  }
  return EpdFontFamily::REGULAR;
}

void MarkdownParser::flushWordBuffer(ParseContext& ctx) {
  if (ctx.wordBufferIndex > 0) {
    ctx.wordBuffer[ctx.wordBufferIndex] = '\0';
    ctx.wordBufferIndex = utf8NormalizeNfc(ctx.wordBuffer, ctx.wordBufferIndex);
    if (ctx.textBlock) {
      ctx.textBlock->addWord(ctx.wordBuffer, static_cast<EpdFontFamily::Style>(getCurrentFontStyle(ctx)));
    }
    ctx.wordBufferIndex = 0;
  }
}

void MarkdownParser::startNewTextBlock(ParseContext& ctx, const int style) {
  if (ctx.hitMaxPages) {
    return;
  }

  if (ctx.textBlock) {
    if (ctx.textBlock->isEmpty()) {
      ctx.textBlock->setStyle(static_cast<TextBlock::BLOCK_STYLE>(style));
      return;
    }
    flushTextBlock(ctx);
    if (ctx.hitMaxPages) {
      return;
    }
  }
  ctx.textBlock.reset(new ParsedText(static_cast<TextBlock::BLOCK_STYLE>(style), config_.indentLevel,
                                     config_.hyphenation, true, isRtl_));
}

void MarkdownParser::flushTextBlock(ParseContext& ctx) {
  flushWordBuffer(ctx);
  if (!ctx.textBlock || ctx.textBlock->isEmpty()) {
    return;
  }

  if (!ctx.currentPage) {
    ctx.currentPage.reset(new Page());
    ctx.pageNextY = 0;
  }

  const int lineHeight = static_cast<int>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression);

  ctx.textBlock->layoutAndExtractLines(
      renderer_, config_.fontId, config_.viewportWidth,
      [this, &ctx](const std::shared_ptr<TextBlock>& textBlock) {
        if (!ctx.hitMaxPages) {
          addLineToPage(ctx, textBlock);
        }
      },
      true, [&ctx]() -> bool { return ctx.hitMaxPages; });

  if (!ctx.hitMaxPages) {
    ctx.textBlock.reset();

    switch (config_.spacingLevel) {
      case 1:
        ctx.pageNextY += lineHeight / 4;
        break;
      case 3:
        ctx.pageNextY += lineHeight;
        break;
    }
  }
  // else: textBlock still has unconsumed words — preserve for next batch
}

bool MarkdownParser::addLineToPage(ParseContext& ctx, std::shared_ptr<TextBlock> line) {
  const int lineHeight = static_cast<int>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression);

  if (ctx.pageNextY + lineHeight > config_.viewportHeight) {
    if (ctx.onPageComplete) {
      const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      LOG_DBG(TAG, "Page %d complete, heap: %zu free", ctx.pagesCreated, freeHeap);
      ctx.onPageComplete(std::move(ctx.currentPage));
      ctx.pagesCreated++;

      if (freeHeap < 12000) {
        LOG_ERR(TAG, "Stopping early due to low memory (%zu bytes)", freeHeap);
        ctx.hitMaxPages = true;
        ctx.currentPage.reset(new Page());
        ctx.pageNextY = 0;
        return false;
      }
    }
    ctx.currentPage.reset(new Page());
    ctx.pageNextY = 0;

    if (ctx.maxPages > 0 && ctx.pagesCreated >= ctx.maxPages) {
      ctx.currentPage->elements.push_back(std::make_unique<PageLine>(std::move(line), 0, ctx.pageNextY));
      ctx.pageNextY += lineHeight;
      ctx.hitMaxPages = true;
      return false;
    }
  }

  ctx.currentPage->elements.push_back(std::make_unique<PageLine>(std::move(line), 0, ctx.pageNextY));
  ctx.pageNextY += lineHeight;
  return true;
}

// v2.0.192 — moved from a MarkdownParser member method to a free
// function so it can take the AnyFile wrapper without exposing
// AnyFile in the public header.  Operates on a caller-provided
// line buffer (was MarkdownParser::lineBuffer_, still passed in
// from the same field).
namespace {
bool readLineAnyFile(AnyFile& file, char* lineBuffer, int bufferSize,
                     int* lineLength, bool* isBlank) {
  const int readLen = file.fgets(lineBuffer, bufferSize);
  if (readLen <= 0) {
    if (lineLength) *lineLength = 0;
    if (isBlank) *isBlank = true;
    return false;
  }

  int len = readLen;
  while (len > 0 && (lineBuffer[len - 1] == '\n' || lineBuffer[len - 1] == '\r')) {
    len--;
  }
  lineBuffer[len] = '\0';

  if (lineLength) {
    *lineLength = len;
  }
  if (isBlank) {
    bool blank = true;
    for (int i = 0; i < len; i++) {
      if (!isWhitespaceChar(lineBuffer[i])) {
        blank = false;
        break;
      }
    }
    *isBlank = blank;
  }
  return true;
}
}  // namespace

void MarkdownParser::appendTextBytes(ParseContext& ctx, const char* data, int len) {
  int offset = 0;
  while (offset < len) {
    while (offset < len && isWhitespaceChar(data[offset])) {
      flushWordBuffer(ctx);
      offset++;
    }

    const int runStart = offset;
    while (offset < len && !isWhitespaceChar(data[offset])) {
      offset++;
    }

    int remaining = offset - runStart;
    const char* src = data + runStart;
    while (remaining > 0) {
      if (ctx.wordBufferIndex >= MAX_WORD_SIZE) {
        flushWordBuffer(ctx);
      }

      const int spaceLeft = MAX_WORD_SIZE - ctx.wordBufferIndex;
      const int chunkLen = utf8SafePrefixLength(src, remaining, spaceLeft);
      memcpy(ctx.wordBuffer + ctx.wordBufferIndex, src, chunkLen);
      ctx.wordBufferIndex += chunkLen;
      src += chunkLen;
      remaining -= chunkLen;

      if (ctx.wordBufferIndex >= MAX_WORD_SIZE) {
        flushWordBuffer(ctx);
      }
    }
  }
}

bool MarkdownParser::tokenCallback(const md_token_t* token, void* userData) {
  auto& ctx = *static_cast<ParseContext*>(userData);
  auto* self = ctx.self;

  if (ctx.hitMaxPages) {
    return false;
  }

  auto flushBeforeCurrentLineContent = [&]() -> bool {
    self->flushTextBlock(ctx);
    if (ctx.hitMaxPages) {
      ctx.replayCurrentLine = true;
      return false;
    }
    return true;
  };

  switch (token->type) {
    case MD_TEXT: {
      self->appendTextBytes(ctx, token->text, token->length);
      break;
    }

    case MD_HEADER_START: {
      if (!flushBeforeCurrentLineContent()) return false;
      ctx.headerLevel = token->data;
      self->startNewTextBlock(ctx, TextBlock::CENTER_ALIGN);
      ctx.inBold = true;
      break;
    }

    case MD_HEADER_END: {
      self->flushTextBlock(ctx);
      ctx.inBold = false;
      ctx.headerLevel = 0;
      break;
    }

    case MD_BOLD_START:
      self->flushWordBuffer(ctx);
      ctx.inBold = true;
      break;

    case MD_BOLD_END:
      self->flushWordBuffer(ctx);
      ctx.inBold = false;
      break;

    case MD_ITALIC_START:
      self->flushWordBuffer(ctx);
      ctx.inItalic = true;
      break;

    case MD_ITALIC_END:
      self->flushWordBuffer(ctx);
      ctx.inItalic = false;
      break;

    case MD_LIST_ITEM_START: {
      if (!flushBeforeCurrentLineContent()) return false;
      self->startNewTextBlock(ctx, TextBlock::LEFT_ALIGN);
      if (token->data > 0) {
        // Ordered list - emit number
        char numBuf[8];
        snprintf(numBuf, sizeof(numBuf), "%d.", token->data);
        ctx.textBlock->addWord(numBuf, EpdFontFamily::REGULAR);
      } else {
        // Unordered list - emit bullet
        ctx.textBlock->addWord("•", EpdFontFamily::REGULAR);
      }
      break;
    }

    case MD_CODE_INLINE: {
      self->flushWordBuffer(ctx);
      // Emit inline code in italic
      bool savedItalic = ctx.inItalic;
      ctx.inItalic = true;
      self->appendTextBytes(ctx, token->text, token->length);
      self->flushWordBuffer(ctx);
      ctx.inItalic = savedItalic;
      break;
    }

    case MD_CODE_BLOCK_START: {
      if (!flushBeforeCurrentLineContent()) return false;
      self->startNewTextBlock(ctx, TextBlock::LEFT_ALIGN);
      ctx.textBlock->addWord("[Code:", EpdFontFamily::ITALIC);
      ctx.inCodeBlock = true;
      break;
    }

    case MD_CODE_BLOCK_END: {
      if (ctx.textBlock) {
        ctx.textBlock->addWord("...]", EpdFontFamily::ITALIC);
      }
      self->flushTextBlock(ctx);
      ctx.inCodeBlock = false;
      break;
    }

    case MD_HR: {
      if (!flushBeforeCurrentLineContent()) return false;
      self->startNewTextBlock(ctx, TextBlock::CENTER_ALIGN);
      ctx.textBlock->addWord("───────────", EpdFontFamily::REGULAR);
      self->flushTextBlock(ctx);
      break;
    }

    case MD_BLOCKQUOTE_START: {
      if (!flushBeforeCurrentLineContent()) return false;
      self->startNewTextBlock(ctx, TextBlock::LEFT_ALIGN);
      ctx.inItalic = true;
      break;
    }

    case MD_BLOCKQUOTE_END:
      self->flushTextBlock(ctx);
      if (ctx.hitMaxPages) {
        ctx.replayCurrentLine = true;
        return false;
      }
      ctx.inItalic = false;
      break;

    case MD_LINK_TEXT_START:
    case MD_LINK_TEXT_END:
    case MD_LINK_URL:
      // Just emit link text, ignore URL (handled via MD_TEXT between LINK_TEXT_START/END)
      break;

    case MD_IMAGE_ALT_START:
    case MD_IMAGE_ALT_END:
    case MD_IMAGE_URL: {
      // Show placeholder for images
      if (token->type == MD_IMAGE_ALT_START) {
        self->flushWordBuffer(ctx);
        if (ctx.textBlock) {
          ctx.textBlock->addWord("[Image]", EpdFontFamily::ITALIC);
        }
      }
      break;
    }

    case MD_NEWLINE: {
      // Newline within a block - just add space
      self->flushWordBuffer(ctx);
      break;
    }

    case MD_STRIKE_START:
    case MD_STRIKE_END:
    case MD_LIST_ITEM_END:
    case MD_PARAGRAPH_START:
    case MD_PARAGRAPH_END:
      // Not used in this implementation
      break;
  }

  return true;
}

bool MarkdownParser::parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages,
                                const AbortCallback& shouldAbort) {
  // v2.0.192 — route through AnyFile so the same loop works whether
  // the file lives on SD (default) or LittleFS (UTF-8 cache for a
  // cp1251 source, generated by Markdown::load()).  `useLittleFs_`
  // is set by the constructor based on the caller's knowledge of the
  // Markdown object's effective content path.
  AnyFile file;
  const bool opened = useLittleFs_ ? file.openLittleFs(filepath_) : file.openSd(filepath_);
  if (!opened) {
    LOG_ERR(TAG, "Failed to open file (%s): %s",
            useLittleFs_ ? "LittleFS" : "SD", filepath_.c_str());
    return false;
  }

  fileSize_ = file.size();
  if (fileSize_ == 0) {
    LOG_ERR(TAG, "Empty markdown file");
    file.close();
    hasMore_ = false;
    return true;
  }

  file.seekSet(currentOffset_);

  if (currentOffset_ == 0 && !isRtl_) {
    int rtlLineLen = 0;
    if (readLineAnyFile(file, lineBuffer_, LINE_BUFFER_SIZE, &rtlLineLen, nullptr)) {
      isRtl_ = ScriptDetector::containsArabic(lineBuffer_);
    }
    file.seekSet(currentOffset_);
  }

  LOG_INF(TAG, "Parsing from offset %zu, file size %zu", currentOffset_, fileSize_);
  LOG_DBG(TAG, "Heap: %zu free", heap_caps_get_free_size(MALLOC_CAP_8BIT));

  // Initialize parsing context
  ParseContext ctx{};
  ctx.self = this;
  ctx.pageNextY = 0;
  ctx.inBold = false;
  ctx.inItalic = false;
  ctx.inCodeBlock = false;
  ctx.headerLevel = 0;
  ctx.hitMaxPages = false;
  ctx.pagesCreated = 0;
  ctx.maxPages = maxPages;
  ctx.replayCurrentLine = false;
  ctx.onPageComplete = onPageComplete;
  ctx.wordBufferIndex = 0;

  auto preservePendingPage = [&]() {
    if (ctx.currentPage && !ctx.currentPage->elements.empty()) {
      pendingPage_ = std::move(ctx.currentPage);
      pendingPageNextY_ = ctx.pageNextY;
    }
  };

  if (pendingPage_) {
    ctx.currentPage = std::move(pendingPage_);
    ctx.pageNextY = pendingPageNextY_;
    pendingPageNextY_ = 0;
  }

  // Initialize md_parser
  md_parser_t parser;
  md_parser_init(&parser, tokenCallback, &ctx);

  // Resume: flush any pending text block carried over from a previous interrupted batch
  if (pendingTextBlock_) {
    ctx.textBlock = std::move(pendingTextBlock_);
    flushTextBlock(ctx);
    if (ctx.hitMaxPages) {
      // Still can't fit — save and return
      pendingTextBlock_ = std::move(ctx.textBlock);
      preservePendingPage();
      hasMore_ = true;
      file.close();
      return true;
    }
  }

  // Start with a paragraph block
  startNewTextBlock(ctx, config_.paragraphAlignment);

  size_t bytesProcessed = 0;
  bool prevLineBlank = true;
  uint16_t abortCheckCounter = 0;

  while (!ctx.hitMaxPages) {
    // Check for external abort every few lines
    if (shouldAbort && (++abortCheckCounter % 20 == 0) && shouldAbort()) {
      LOG_INF(TAG, "Aborted by external request");
      ctx.hitMaxPages = true;
      break;
    }

    const size_t lineStartOffset = file.position();
    int lineLen = 0;
    bool isBlank = true;
    if (!readLineAnyFile(file, lineBuffer_, LINE_BUFFER_SIZE, &lineLen, &isBlank)) {
      break;
    }
    const size_t lineEndOffset = file.position();
    bytesProcessed = lineEndOffset - currentOffset_;
    ctx.replayCurrentLine = false;

    if (isBlank) {
      if (!prevLineBlank && !ctx.inCodeBlock) {
        flushTextBlock(ctx);
        if (ctx.hitMaxPages) {
          break;
        }
        startNewTextBlock(ctx, config_.paragraphAlignment);
      }
      prevLineBlank = true;
      continue;
    }

    // Reset parser state for each line but preserve formatting state
    md_parser_reset(&parser);

    // Parse the line
    md_parse(&parser, lineBuffer_, lineLen);
    if (ctx.hitMaxPages) {
      if (ctx.replayCurrentLine) {
        bytesProcessed = lineStartOffset - currentOffset_;
      }
      break;
    }

    prevLineBlank = false;

    // Periodic memory check
    if (ctx.textBlock && ctx.textBlock->size() > 300) {
      const size_t freeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      if (freeBlock < 25000) {
        LOG_ERR(TAG, "Low memory (%zu free), flushing early", freeBlock);
        ctx.textBlock->layoutAndExtractLines(
            renderer_, config_.fontId, config_.viewportWidth,
            [this, &ctx](const std::shared_ptr<TextBlock>& textBlock) {
              if (!ctx.hitMaxPages) {
                addLineToPage(ctx, textBlock);
              }
            },
            false, [&ctx]() -> bool { return ctx.hitMaxPages; });
      }
    }
  }

  file.close();

  // Finalize
  flushTextBlock(ctx);

  // Save any unconsumed text block for the next parsePages call
  if (ctx.hitMaxPages) {
    if (ctx.textBlock && !ctx.textBlock->isEmpty()) {
      pendingTextBlock_ = std::move(ctx.textBlock);
    } else {
      pendingTextBlock_.reset();
    }
    preservePendingPage();
  } else {
    pendingTextBlock_.reset();
    if (ctx.currentPage && !ctx.currentPage->elements.empty() && onPageComplete) {
      onPageComplete(std::move(ctx.currentPage));
      ctx.pagesCreated++;
    }
  }

  currentOffset_ += bytesProcessed;
  hasMore_ = ctx.hitMaxPages || pendingPage_ || (currentOffset_ < fileSize_);

  LOG_INF(TAG, "Parsed %d pages, offset %zu/%zu, hasMore=%d", ctx.pagesCreated, currentOffset_, fileSize_, hasMore_);
  return true;
}
