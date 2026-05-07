#include "Fb2Parser.h"

#include "Fb2.h"
#include <GfxRenderer.h>
#include <Logging.h>
#include <Page.h>
#include <ParsedText.h>
#include <SDCardManager.h>
#include <SharedSpiLock.h>
#include <Utf8.h>
#include <blocks/ImageBlock.h>

#define TAG "FB2_PARSE"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {
constexpr size_t READ_CHUNK_SIZE = 2048;

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
                     const int startingSectionIndex, const bool sectionScoped, const uint32_t endOffset)
    : filepath_(std::move(filepath)),
      renderer_(renderer),
      config_(config),
      startOffset_(startOffset),
      endOffset_(endOffset),
      startingSectionIndex_(startingSectionIndex),
      sectionScoped_(sectionScoped) {}

Fb2Parser::~Fb2Parser() { reset(); }

void Fb2Parser::releaseStreamingState() {
  if (xmlParser_) {
    XML_ParserFree(xmlParser_);
    xmlParser_ = nullptr;
  }
  if (file_) {
    snapix::spi::SharedBusLock lk;
    file_.close();
  }
  initialized_ = false;
  suspended_ = false;
  xmlParserSuspended_ = false;
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
  xmlParserSuspended_ = false;
  pendingNewTextBlock_ = false;
  pendingBlockStyle_ = TextBlock::LEFT_ALIGN;
  pendingSectionStart_ = false;
  pendingSectionNeedsPageBreak_ = false;
  pendingSectionAnchorIndex_ = -1;
  delete[] partWordBuffer_;
  partWordBuffer_ = nullptr;
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

void Fb2Parser::requestXmlSuspend() {
  hitMaxPages_ = true;
  stopRequested_ = true;
  if (xmlParser_ && !xmlParserSuspended_) {
    XML_StopParser(xmlParser_, XML_TRUE);
    xmlParserSuspended_ = true;
  }
}

bool Fb2Parser::finishPendingSectionStart() {
  if (!pendingSectionStart_) {
    return true;
  }

  const bool needsPageBreak = pendingSectionNeedsPageBreak_;
  const int anchorIndex = pendingSectionAnchorIndex_;

  if (needsPageBreak) {
    if (currentPage_ && !currentPage_->elements.empty()) {
      onPageComplete_(std::move(currentPage_));
      pagesCreated_++;
      if (maxPages_ > 0 && pagesCreated_ >= maxPages_) {
        requestXmlSuspend();
        return false;
      }
    }
    startNewPage();
  }

  firstSection_ = false;
  if (anchorIndex >= 0) {
    anchorMap_.emplace_back("section_" + std::to_string(anchorIndex), pagesCreated_);
  }

  pendingSectionStart_ = false;
  pendingSectionNeedsPageBreak_ = false;
  pendingSectionAnchorIndex_ = -1;
  return true;
}

bool Fb2Parser::flushDeferredLayoutBeforeResume() {
  if (currentTextBlock_ && !currentTextBlock_->isEmpty()) {
    makePages();
    if (stopRequested_) {
      suspended_ = true;
      hasMore_ = true;
      return false;
    }
  }

  if (!finishPendingSectionStart()) {
    suspended_ = true;
    hasMore_ = true;
    return false;
  }

  if (pendingNewTextBlock_) {
    pendingNewTextBlock_ = false;
    currentTextBlock_.reset(new ParsedText(pendingBlockStyle_, config_.indentLevel, config_.hyphenation, true, isRtl_));
  }

  return true;
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

    {
      snapix::spi::SharedBusLock lk;
      fileSize_ = file_.size();
      lastParsedOffset_ = startOffset_;

      if (startOffset_ > 0) {
        file_.seek(startOffset_);
      }
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

    // A previous batch may have stopped while laying out an already parsed
    // paragraph.  Finish that tail before Expat is resumed, otherwise new XML
    // characterData can be appended to the old ParsedText and shift/duplicate
    // page-boundary text.
    if (!flushDeferredLayoutBeforeResume()) {
      return true;
    }

    // The Expat parser may have been suspended mid-buffer via
    // XML_StopParser(resumable) when hitMaxPages fired.  Finish
    // processing the remainder of that buffer before reading new data.
    if (xmlParserSuspended_) {
      xmlParserSuspended_ = false;
      const XML_Status rs = XML_ResumeParser(xmlParser_);
      if (rs == XML_STATUS_ERROR) {
        const XML_Error ec = XML_GetErrorCode(xmlParser_);
        if (!(fragmentComplete_ && ec == XML_ERROR_ABORTED)) {
          LOG_ERR(TAG, "Resume parse error at line %lu: %s",
                  XML_GetCurrentLineNumber(xmlParser_), XML_ErrorString(ec));
          releaseStreamingState();
          currentTextBlock_.reset();
          currentPage_.reset();
          partWordBufferIndex_ = 0;
          return false;
        }
        // fragmentComplete_ during resume — fall through to flush below
      } else if (stopRequested_) {
        // hitMaxPages fired again during resumed processing
        suspended_ = true;
        hasMore_ = true;
        return true;
      }
    }
  }

  // If the section was fully parsed during the resumed buffer,
  // skip the main read loop and go straight to flush/finalize.
  if (!fragmentComplete_) {
    // Single buffer reused for parsing (saves stack)
    uint8_t buffer[READ_CHUNK_SIZE + 1];
    uint16_t abortCheckCounter = 0;

    while (true) {
      // --- SPI-locked section: read a chunk from SD card ---
      int bytesRead;
      int done;
      {
        snapix::spi::SharedBusLock lk;
        if (file_.available() <= 0) break;

        size_t bytesToRead = READ_CHUNK_SIZE;
        if (sectionScoped_ && endOffset_ > startOffset_) {
          const size_t pos = file_.position();
          if (pos >= endOffset_) {
            fragmentComplete_ = true;
            break;
          }
          bytesToRead = std::min(bytesToRead, static_cast<size_t>(endOffset_ - pos));
        }

        bytesRead = file_.read(buffer, bytesToRead);
        if (bytesRead <= 0) {
          if (bytesRead < 0) {
            LOG_ERR(TAG, "SD read error at offset %lu", static_cast<unsigned long>(file_.position()));
          }
          break;
        }
        done = (file_.available() == 0 && !(sectionScoped_ && endOffset_ > startOffset_)) ? 1 : 0;
      }
      // --- SPI released: display driver can use the bus while we process XML ---

      if (shouldAbort_ && (++abortCheckCounter % 10 == 0) && shouldAbort_()) {
        LOG_INF(TAG, "Aborted by external request");
        releaseStreamingState();
        currentTextBlock_.reset();
        currentPage_.reset();
        partWordBufferIndex_ = 0;
        hasMore_ = true;
        return false;
      }

      if (XML_Parse(xmlParser_, reinterpret_cast<const char*>(buffer), bytesRead, done) ==
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

      {
        snapix::spi::SharedBusLock lk;
        lastParsedOffset_ = static_cast<uint32_t>(std::min<size_t>(file_.position(), fileSize_));
        if (sectionScoped_ && endOffset_ > startOffset_ && file_.position() >= endOffset_) {
          fragmentComplete_ = true;
        }
      }

      if (stopRequested_) {
        suspended_ = true;
        hasMore_ = true;
        return true;
      }

      if (fragmentComplete_) {
        break;
      }
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
    const bool needsPageBreak = !self->firstSection_;
    self->sectionCounter_++;
    const int sectionAnchorIndex = self->sectionCounter_ - 1;
    if (self->sectionScoped_) {
      self->targetSectionStarted_ = true;
      self->targetSectionDepth_++;
    }
    if (needsPageBreak) {
      // Flush current content before new section
      self->flushPartWordBuffer();
      if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
        self->makePages();
        if (self->stopRequested_) {
          self->pendingSectionStart_ = true;
          self->pendingSectionNeedsPageBreak_ = true;
          self->pendingSectionAnchorIndex_ = sectionAnchorIndex;
          self->depth_++;
          return;
        }
      }
    }

    self->pendingSectionStart_ = true;
    self->pendingSectionNeedsPageBreak_ = needsPageBreak;
    self->pendingSectionAnchorIndex_ = sectionAnchorIndex;
    if (!self->finishPendingSectionStart()) {
      self->depth_++;
      return;
    }
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
    // Flush any partial word collected under the *outer* (non-italic) style
    // before italic takes effect.  Otherwise text like "abc<emphasis>def"
    // would emit "abcdef" attributed to whichever style is active at the
    // next flush boundary.
    if (self->partWordBufferIndex_ > 0) {
      self->flushPartWordBuffer();
    }
    self->italicUntilDepth_ = std::min(self->italicUntilDepth_, self->depth_);
  } else if (strcmp(localName, "strong") == 0) {
    if (self->partWordBufferIndex_ > 0) {
      self->flushPartWordBuffer();
    }
    self->boldUntilDepth_ = std::min(self->boldUntilDepth_, self->depth_);
  } else if (strcmp(localName, "empty-line") == 0) {
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
      if (self->stopRequested_) {
        self->depth_++;
        return;
      }
    }
    self->addVerticalSpacing(1);
  } else if (strcmp(localName, "image") == 0) {
    // Inline FB2 image (<image l:href="#binary_id"/>).  When an Fb2 instance
    // is wired (setFb2) and showImages is on, materialise the referenced
    // <binary> base64 block into a cached BMP and emit it as a PageImage.
    // Otherwise silently skip — keeps legacy behaviour intact.
    if (!self->fb2_ || !self->config_.showImages || self->stopRequested_) return;

    const char* href = nullptr;
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        const char* an = atts[i];
        const char* p = strrchr(an, ':');
        p = p ? (p + 1) : an;
        if (strcmp(p, "href") == 0) {
          href = atts[i + 1];
          break;
        }
      }
    }
    if (!href || href[0] != '#' || !href[1]) return;
    std::string binaryId(href + 1);

    // Cap output dimensions to the viewport so the JPEG is downscaled by the
    // converter rather than blown up at render time.  Leave room for at
    // least a couple of text lines around the image.
    const int maxW = std::max(64, static_cast<int>(self->config_.viewportWidth) - 12);
    const int maxH = std::max(64, static_cast<int>(self->config_.viewportHeight) - 80);

    std::string bmpPath;
    uint16_t w = 0, h = 0;
    if (!self->fb2_->cacheImage(binaryId, bmpPath, w, h, maxW, maxH) || w == 0 || h == 0) {
      LOG_DBG(TAG, "image <%s>: cache miss, falling back to skip", binaryId.c_str());
      return;
    }

    auto imageBlock = std::make_shared<ImageBlock>(bmpPath, w, h, /*nodeId*/ "", binaryId, /*resolved*/ "");

    // Flush the current text run so the image lands on its own paragraph.
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
      if (self->stopRequested_) return;
    }
    self->addImageToPage(std::move(imageBlock));
  }

  self->depth_++;
}

void XMLCALL Fb2Parser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<Fb2Parser*>(userData);
  const char* localName = stripNamespace(name);

  self->depth_--;

  // If the closing tag is about to turn bold or italic OFF, flush any partial
  // word still in the buffer *before* clearing the style anchor — otherwise
  // the last word inside `<strong>...</strong>` (with no trailing whitespace,
  // e.g. when followed immediately by `</p>`) gets emitted under the post-tag
  // style.  This is what makes the last bold word in a line render as regular
  // under fakeBold.
  const bool willClearBold = self->depth_ <= self->boldUntilDepth_;
  const bool willClearItalic = self->depth_ <= self->italicUntilDepth_;
  if ((willClearBold || willClearItalic) && self->partWordBufferIndex_ > 0) {
    self->flushPartWordBuffer();
  }

  if (willClearBold) {
    self->boldUntilDepth_ = INT_MAX;
  }
  if (willClearItalic) {
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
      if (self->stopRequested_) {
        return;
      }
    }
    self->addVerticalSpacing(1);
  } else if (strcmp(localName, "subtitle") == 0) {
    self->inSubtitle_ = false;
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
      if (self->stopRequested_) {
        return;
      }
    }
    self->addVerticalSpacing(1);
  } else if (strcmp(localName, "p") == 0) {
    self->inParagraph_ = false;
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
      if (self->stopRequested_) {
        return;
      }
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
  if (!partWordBuffer_) {
    partWordBuffer_ = new char[MAX_WORD_SIZE + 1];
    partWordBufferIndex_ = 0;
  }
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
  if (stopRequested_) {
    pendingNewTextBlock_ = true;
    pendingBlockStyle_ = style;
    return;
  }

  if (currentTextBlock_) {
    if (currentTextBlock_->isEmpty()) {
      currentTextBlock_->setStyle(style);
      return;
    }
    makePages();
    if (stopRequested_) {
      pendingNewTextBlock_ = true;
      pendingBlockStyle_ = style;
      return;
    }
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
  if (stopRequested_) {
    return;
  }

  const int lineHeight = static_cast<int>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression);

  if (!currentPage_) {
    startNewPage();
  }

  if (currentPageNextY_ + lineHeight > config_.viewportHeight) {
    onPageComplete_(std::move(currentPage_));
    pagesCreated_++;
    startNewPage();

    if (maxPages_ > 0 && pagesCreated_ >= maxPages_) {
      requestXmlSuspend();
    }
  }

  currentPage_->elements.push_back(std::make_unique<PageLine>(std::move(line), 0, currentPageNextY_));
  currentPageNextY_ += lineHeight;
}

void Fb2Parser::addImageToPage(std::shared_ptr<ImageBlock> image) {
  if (!image || stopRequested_) return;

  const int imageHeight = image->getHeight();
  const int imageWidth = image->getWidth();
  if (imageHeight <= 0 || imageWidth <= 0) return;

  if (!currentPage_) {
    startNewPage();
  }

  // If the image won't fit in the remaining space on the current page,
  // complete the current page first.  This avoids clipping at the bottom.
  if (currentPageNextY_ + imageHeight > config_.viewportHeight) {
    if (currentPage_ && !currentPage_->elements.empty()) {
      onPageComplete_(std::move(currentPage_));
      pagesCreated_++;
      if (maxPages_ > 0 && pagesCreated_ >= maxPages_) {
        hitMaxPages_ = true;
        requestXmlSuspend();
        return;
      }
    }
    startNewPage();
  }

  // Centre the image horizontally within the viewport.  When the cached BMP
  // is wider than the viewport (shouldn't happen given cacheImage's downscale
  // but defensive), clamp xPos to 0.
  int xPos = (static_cast<int>(config_.viewportWidth) - imageWidth) / 2;
  if (xPos < 0) xPos = 0;

  const int yPos = currentPageNextY_;
  currentPage_->elements.push_back(std::make_unique<PageImage>(std::move(image), xPos, yPos));

  // Add a single line height of breathing room below the image so the next
  // paragraph doesn't bump into it.
  const int lineHeight = std::max(8, static_cast<int>(renderer_.getLineHeight(config_.fontId) * config_.lineCompression));
  currentPageNextY_ = static_cast<int16_t>(std::min(yPos + imageHeight + lineHeight, 32767));
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
