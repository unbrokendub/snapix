#include "Fb2Parser.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Page.h>
#include <ParsedText.h>
#include <SDCardManager.h>
#include <Utf8.h>

#define TAG "FB2_PARSE"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {
constexpr size_t READ_CHUNK_SIZE = 4096;

bool isWhitespace(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

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

const char* stripNamespace(const char* name) {
  const char* local = strrchr(name, ':');
  return local ? local + 1 : name;
}
}  // namespace

Fb2Parser::Fb2Parser(std::string filepath, GfxRenderer& renderer, const RenderConfig& config, const uint32_t startOffset,
                     const int startingSectionIndex, const bool sectionScoped)
    : filepath_(std::move(filepath)),
      renderer_(renderer),
      config_(config),
      startOffset_(startOffset),
      startingSectionIndex_(startingSectionIndex),
      sectionScoped_(sectionScoped) {}

Fb2Parser::~Fb2Parser() { reset(); }

void Fb2Parser::releaseStreamingState() {
  if (xmlParser_) {
    XML_ParserFree(xmlParser_);
    xmlParser_ = nullptr;
  }
  if (file_) {
    file_.close();
  }
  initialized_ = false;
  suspended_ = false;
}

void Fb2Parser::reset() {
  releaseStreamingState();
  hasMore_ = true;
  isRtl_ = false;
  stopRequested_ = false;
  depth_ = 0;
  skipUntilDepth_ = INT_MAX;
  boldUntilDepth_ = INT_MAX;
  italicUntilDepth_ = INT_MAX;
  inBody_ = false;
  inTitle_ = false;
  inSubtitle_ = false;
  inParagraph_ = false;
  bodyCount_ = 0;
  sectionCounter_ = startingSectionIndex_;
  firstSection_ = true;
  targetSectionStarted_ = false;
  targetSectionDepth_ = 0;
  fragmentComplete_ = false;
  partWordBufferIndex_ = 0;
  rtlArabicWords_ = 0;
  rtlLtrWords_ = 0;
  currentTextBlock_.reset();
  currentPage_.reset();
  currentPageNextY_ = 0;
  pagesCreated_ = 0;
  hitMaxPages_ = false;
  fileSize_ = 0;
  lastParsedOffset_ = startOffset_;
  anchorMap_.clear();
}

bool Fb2Parser::parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages,
                           const AbortCallback& shouldAbort) {
  onPageComplete_ = onPageComplete;
  maxPages_ = maxPages;
  pagesCreated_ = 0;
  hitMaxPages_ = false;
  stopRequested_ = false;
  shouldAbort_ = shouldAbort;

  if (!canResume()) {
    reset();

    if (!SdMan.openFileForRead("FB2", filepath_, file_)) {
      LOG_ERR(TAG, "Failed to open file: %s", filepath_.c_str());
      return false;
    }

    fileSize_ = file_.size();
    lastParsedOffset_ = startOffset_;

    if (startOffset_ > 0) {
      file_.seek(startOffset_);
    }

    xmlParser_ = XML_ParserCreate("UTF-8");
    if (!xmlParser_) {
      LOG_ERR(TAG, "Failed to create XML parser");
      releaseStreamingState();
      return false;
    }

    XML_SetUserData(xmlParser_, this);
    XML_SetElementHandler(xmlParser_, startElement, endElement);
    XML_SetCharacterDataHandler(xmlParser_, characterData);

    startNewPage();
    if (startOffset_ > 0) {
      constexpr char kSyntheticPrefix[] = "<FictionBook><body>";
      if (XML_Parse(xmlParser_, kSyntheticPrefix, static_cast<int>(sizeof(kSyntheticPrefix) - 1), 0) ==
          XML_STATUS_ERROR) {
        LOG_ERR(TAG, "Failed to initialize section parser");
        releaseStreamingState();
        return false;
      }
    }
    initialized_ = true;
  } else {
    suspended_ = false;
  }

  // Single buffer reused for parsing (saves stack)
  uint8_t buffer[READ_CHUNK_SIZE + 1];
  uint16_t abortCheckCounter = 0;

  while (file_.available() > 0) {
    if (shouldAbort_ && (++abortCheckCounter % 10 == 0) && shouldAbort_()) {
      LOG_INF(TAG, "Aborted by external request");
      releaseStreamingState();
      currentTextBlock_.reset();
      currentPage_.reset();
      partWordBufferIndex_ = 0;
      hasMore_ = true;
      return false;
    }

    size_t bytesRead = file_.read(buffer, READ_CHUNK_SIZE);
    if (bytesRead == 0) break;

    int done = (file_.available() == 0) ? 1 : 0;
    if (XML_Parse(xmlParser_, reinterpret_cast<const char*>(buffer), static_cast<int>(bytesRead), done) ==
        XML_STATUS_ERROR) {
      if (!(fragmentComplete_ && XML_GetErrorCode(xmlParser_) == XML_ERROR_ABORTED)) {
        LOG_ERR(TAG, "Parse error at line %lu: %s", XML_GetCurrentLineNumber(xmlParser_),
                XML_ErrorString(XML_GetErrorCode(xmlParser_)));
        releaseStreamingState();
        currentTextBlock_.reset();
        currentPage_.reset();
        partWordBufferIndex_ = 0;
        return false;
      }
      break;
    }

    lastParsedOffset_ = static_cast<uint32_t>(std::min<size_t>(file_.position(), fileSize_));

    if (stopRequested_) {
      suspended_ = true;
      hasMore_ = true;
      return true;
    }

    if (fragmentComplete_) {
      break;
    }
  }

  // Flush remaining content
  flushPartWordBuffer();
  if (currentTextBlock_ && !currentTextBlock_->isEmpty()) {
    makePages();
    if (stopRequested_) {
      suspended_ = true;
      hasMore_ = true;
      return true;
    }
  }

  // Emit final page
  if (currentPage_ && !currentPage_->elements.empty()) {
    onPageComplete_(std::move(currentPage_));
    pagesCreated_++;
  }

  releaseStreamingState();
  currentTextBlock_.reset();
  currentPage_.reset();
  hasMore_ = false;

  LOG_INF(TAG, "Parsed %d pages from %s", pagesCreated_, filepath_.c_str());
  return true;
}

void XMLCALL Fb2Parser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<Fb2Parser*>(userData);
  const char* localName = stripNamespace(name);

  // Prevent stack overflow from deeply nested XML
  if (self->depth_ >= 100) {
    self->depth_++;
    return;
  }

  if (self->skipUntilDepth_ < self->depth_) {
    self->depth_++;
    return;
  }

  if (strcmp(localName, "binary") == 0) {
    self->skipUntilDepth_ = self->depth_;
    self->depth_++;
    return;
  }

  if (strcmp(localName, "body") == 0) {
    self->bodyCount_++;
    self->inBody_ = (self->bodyCount_ == 1);
    self->depth_++;
    return;
  }

  if (!self->inBody_) {
    self->depth_++;
    return;
  }

  if (strcmp(localName, "section") == 0) {
    self->sectionCounter_++;
    if (self->sectionScoped_) {
      self->targetSectionStarted_ = true;
      self->targetSectionDepth_++;
    }
    if (!self->firstSection_) {
      // Flush current content before new section
      self->flushPartWordBuffer();
      if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
        self->makePages();
      }
      // Section break: start new page
      if (self->currentPage_ && !self->currentPage_->elements.empty()) {
        self->onPageComplete_(std::move(self->currentPage_));
        self->pagesCreated_++;
        if (self->maxPages_ > 0 && self->pagesCreated_ >= self->maxPages_) {
          self->hitMaxPages_ = true;
          self->stopRequested_ = true;
          self->depth_++;
          return;
        }
      }
      self->startNewPage();
    }
    self->firstSection_ = false;
    // Record anchor for TOC navigation: section_N → page where this section starts
    self->anchorMap_.emplace_back("section_" + std::to_string(self->sectionCounter_ - 1), self->pagesCreated_);
  } else if (strcmp(localName, "title") == 0) {
    self->inTitle_ = true;
    self->boldUntilDepth_ = std::min(self->boldUntilDepth_, self->depth_);
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->startNewTextBlock(TextBlock::CENTER_ALIGN);
  } else if (strcmp(localName, "subtitle") == 0) {
    self->inSubtitle_ = true;
    self->boldUntilDepth_ = std::min(self->boldUntilDepth_, self->depth_);
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->startNewTextBlock(TextBlock::CENTER_ALIGN);
  } else if (strcmp(localName, "p") == 0) {
    self->inParagraph_ = true;
    if (!self->currentTextBlock_) {
      TextBlock::BLOCK_STYLE style = self->inTitle_ || self->inSubtitle_
                                         ? TextBlock::CENTER_ALIGN
                                         : static_cast<TextBlock::BLOCK_STYLE>(self->config_.paragraphAlignment);
      self->startNewTextBlock(style);
    }
  } else if (strcmp(localName, "emphasis") == 0) {
    self->italicUntilDepth_ = std::min(self->italicUntilDepth_, self->depth_);
  } else if (strcmp(localName, "strong") == 0) {
    self->boldUntilDepth_ = std::min(self->boldUntilDepth_, self->depth_);
  } else if (strcmp(localName, "empty-line") == 0) {
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->addVerticalSpacing(1);
  } else if (strcmp(localName, "image") == 0) {
    // Skip images in v1
  }

  self->depth_++;
}

void XMLCALL Fb2Parser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<Fb2Parser*>(userData);
  const char* localName = stripNamespace(name);

  self->depth_--;

  // Check bold/italic depth AFTER decrementing (depth_ now matches startElement's value)
  if (self->depth_ <= self->boldUntilDepth_) {
    self->boldUntilDepth_ = INT_MAX;
  }
  if (self->depth_ <= self->italicUntilDepth_) {
    self->italicUntilDepth_ = INT_MAX;
  }

  if (self->skipUntilDepth_ == self->depth_) {
    self->skipUntilDepth_ = INT_MAX;
    return;
  }

  if (!self->inBody_) {
    if (strcmp(localName, "body") == 0) {
      // Closing body tag — nothing more to do
    }
    return;
  }

  if (strcmp(localName, "body") == 0) {
    self->inBody_ = false;
    return;
  }

  if (strcmp(localName, "title") == 0) {
    self->inTitle_ = false;
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->addVerticalSpacing(1);
  } else if (strcmp(localName, "subtitle") == 0) {
    self->inSubtitle_ = false;
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->addVerticalSpacing(1);
  } else if (strcmp(localName, "p") == 0) {
    self->inParagraph_ = false;
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
  } else if (strcmp(localName, "section") == 0 && self->sectionScoped_ && self->targetSectionStarted_) {
    if (self->targetSectionDepth_ > 0) {
      self->targetSectionDepth_--;
    }
    if (self->targetSectionDepth_ == 0 && self->xmlParser_) {
      self->fragmentComplete_ = true;
      XML_StopParser(self->xmlParser_, XML_FALSE);
    }
  }
}

void XMLCALL Fb2Parser::characterData(void* userData, const XML_Char* s, int len) {
  auto* self = static_cast<Fb2Parser*>(userData);

  if (self->skipUntilDepth_ < self->depth_) return;
  if (!self->inBody_) return;

  int offset = 0;
  while (offset < len) {
    while (offset < len && isWhitespace(s[offset])) {
      if (self->partWordBufferIndex_ > 0) {
        self->flushPartWordBuffer();
      }
      offset++;
    }

    const int runStart = offset;
    while (offset < len && !isWhitespace(s[offset])) {
      offset++;
    }

    if (offset > runStart) {
      self->appendPartWordBytes(s + runStart, offset - runStart);
    }
  }
}

void Fb2Parser::appendPartWordBytes(const char* data, int len) {
  int remaining = len;
  const char* src = data;

  while (remaining > 0) {
    if (partWordBufferIndex_ >= MAX_WORD_SIZE) {
      flushPartWordBuffer();
    }

    const int spaceLeft = MAX_WORD_SIZE - partWordBufferIndex_;
    const int chunkLen = utf8SafePrefixLength(src, remaining, spaceLeft);
    memcpy(partWordBuffer_ + partWordBufferIndex_, src, chunkLen);
    partWordBufferIndex_ += chunkLen;
    src += chunkLen;
    remaining -= chunkLen;

    if (partWordBufferIndex_ >= MAX_WORD_SIZE) {
      flushPartWordBuffer();
    }
  }
}

void Fb2Parser::flushPartWordBuffer() {
  if (!currentTextBlock_ || partWordBufferIndex_ == 0) {
    partWordBufferIndex_ = 0;
    return;
  }

  partWordBuffer_[partWordBufferIndex_] = '\0';
  partWordBufferIndex_ = static_cast<int>(utf8NormalizeNfc(partWordBuffer_, partWordBufferIndex_));
  observeTextDirectionSample(partWordBuffer_);
  refreshTextDirection();
  currentTextBlock_->addWord(partWordBuffer_, getCurrentFontFamily());
  partWordBufferIndex_ = 0;
}

void Fb2Parser::observeTextDirectionSample(const char* word) {
  if (!word || !*word) {
    return;
  }

  switch (ScriptDetector::classify(word)) {
    case ScriptDetector::Script::ARABIC:
      if (rtlArabicWords_ < UINT16_MAX) {
        rtlArabicWords_++;
      }
      break;
    case ScriptDetector::Script::LATIN:
      if (rtlLtrWords_ < UINT16_MAX) {
        rtlLtrWords_++;
      }
      break;
    default:
      break;
  }
}

void Fb2Parser::refreshTextDirection() {
  const uint16_t strongWordCount = rtlArabicWords_ + rtlLtrWords_;
  if (strongWordCount < 4 && !(rtlArabicWords_ >= 2 && rtlLtrWords_ == 0)) {
    return;
  }

  isRtl_ = rtlArabicWords_ > rtlLtrWords_;
  if (currentTextBlock_) {
    currentTextBlock_->setRtl(isRtl_);
  }
}

void Fb2Parser::startNewTextBlock(TextBlock::BLOCK_STYLE style) {
  if (currentTextBlock_) {
    if (currentTextBlock_->isEmpty()) {
      currentTextBlock_->setStyle(style);
      return;
    }
    makePages();
  }
  currentTextBlock_.reset(new ParsedText(style, config_.indentLevel, config_.hyphenation, true, isRtl_));
}

void Fb2Parser::makePages() {
  if (!currentTextBlock_ || currentTextBlock_->isEmpty()) return;

  flushPartWordBuffer();
  refreshTextDirection();

  if (!currentPage_) {
    startNewPage();
  }

  const int lineHeight = static_cast<int>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression);
  bool continueProcessing = true;

  currentTextBlock_->layoutAndExtractLines(
      renderer_, config_.fontId, config_.viewportWidth,
      [this, &continueProcessing](const std::shared_ptr<TextBlock>& line) {
        if (!continueProcessing) return;
        addLineToPage(line);
        if (hitMaxPages_) {
          continueProcessing = false;
        }
      },
      true, [&continueProcessing]() -> bool { return !continueProcessing; });

  // Paragraph spacing (same pattern as PlainTextParser/ChapterHtmlSlimParser)
  if (!hitMaxPages_) {
    switch (config_.spacingLevel) {
      case 1:
        currentPageNextY_ += lineHeight / 4;
        break;
      case 3:
        currentPageNextY_ += lineHeight;
        break;
    }
    currentTextBlock_.reset();
  }
  // else: currentTextBlock_ still has unconsumed words — preserve for next batch
}

void Fb2Parser::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int lineHeight = static_cast<int>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression);

  if (!currentPage_) {
    startNewPage();
  }

  if (currentPageNextY_ + lineHeight > config_.viewportHeight) {
    onPageComplete_(std::move(currentPage_));
    pagesCreated_++;
    startNewPage();

    if (maxPages_ > 0 && pagesCreated_ >= maxPages_) {
      hitMaxPages_ = true;
      stopRequested_ = true;
    }
  }

  currentPage_->elements.push_back(std::make_unique<PageLine>(std::move(line), 0, currentPageNextY_));
  currentPageNextY_ += lineHeight;
}

void Fb2Parser::startNewPage() {
  currentPage_.reset(new Page());
  currentPageNextY_ = 0;
}

EpdFontFamily::Style Fb2Parser::getCurrentFontFamily() const {
  bool bold = (boldUntilDepth_ < INT_MAX);
  bool italic = (italicUntilDepth_ < INT_MAX);
  if (bold && italic) return EpdFontFamily::BOLD_ITALIC;
  if (bold) return EpdFontFamily::BOLD;
  if (italic) return EpdFontFamily::ITALIC;
  return EpdFontFamily::REGULAR;
}

void Fb2Parser::addVerticalSpacing(int lines) {
  const int lineHeight = static_cast<int>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression);
  currentPageNextY_ += lineHeight * lines;
}
