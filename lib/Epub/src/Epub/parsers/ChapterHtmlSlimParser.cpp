#include "ChapterHtmlSlimParser.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <ImageConverter.h>
#include <Logging.h>
#include <Page.h>
#include <SDCardManager.h>
#include <SharedSpiLock.h>
#include <Utf8.h>
#include <esp_heap_caps.h>
#include <expat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#define TAG "HTML_PARSER"

#include "../htmlEntities.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Minimum file size (in bytes) to show progress bar - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_PROGRESS = 50 * 1024;  // 50KB

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote", "question", "answer", "quotation"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* IMAGE_TAGS[] = {"img"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

enum TagFlags : uint16_t {
  TAG_None = 0,
  TAG_Header = 1 << 0,
  TAG_Block = 1 << 1,
  TAG_Bold = 1 << 2,
  TAG_Italic = 1 << 3,
  TAG_Image = 1 << 4,
  TAG_Skip = 1 << 5,
  TAG_ListOrdered = 1 << 6,
  TAG_ListUnordered = 1 << 7,
  TAG_LineBreak = 1 << 8,
  TAG_ListItem = 1 << 9,
};

inline TagFlags operator|(TagFlags lhs, TagFlags rhs) {
  return static_cast<TagFlags>(static_cast<uint16_t>(lhs) | static_cast<uint16_t>(rhs));
}

inline bool hasTagFlag(TagFlags flags, TagFlags flag) {
  return (static_cast<uint16_t>(flags) & static_cast<uint16_t>(flag)) != 0;
}

TagFlags classifyTag(const char* name) {
  switch (name[0]) {
    case 'a':
      if (strcmp(name, "answer") == 0) return TAG_Block;
      return TAG_None;
    case 'b':
      if (strcmp(name, "b") == 0) return TAG_Bold;
      if (strcmp(name, "br") == 0) return TAG_Block | TAG_LineBreak;
      if (strcmp(name, "blockquote") == 0) return TAG_Block;
      return TAG_None;
    case 'd':
      if (strcmp(name, "div") == 0) return TAG_Block;
      return TAG_None;
    case 'e':
      if (strcmp(name, "em") == 0) return TAG_Italic;
      return TAG_None;
    case 'f':
      if (strcmp(name, "figcaption") == 0) return TAG_Block;
      return TAG_None;
    case 'h':
      if (strcmp(name, "head") == 0) return TAG_Skip;
      if (name[1] >= '1' && name[1] <= '6' && name[2] == '\0') return TAG_Header;
      return TAG_None;
    case 'i':
      if (strcmp(name, "i") == 0) return TAG_Italic;
      if (strcmp(name, "img") == 0) return TAG_Image;
      return TAG_None;
    case 'l':
      if (strcmp(name, "li") == 0) return TAG_Block | TAG_ListItem;
      return TAG_None;
    case 'o':
      if (strcmp(name, "ol") == 0) return TAG_ListOrdered;
      return TAG_None;
    case 'p':
      if (strcmp(name, "p") == 0) return TAG_Block;
      return TAG_None;
    case 'q':
      if (strcmp(name, "question") == 0 || strcmp(name, "quotation") == 0) return TAG_Block;
      return TAG_None;
    case 's':
      if (strcmp(name, "strong") == 0) return TAG_Bold;
      return TAG_None;
    case 'u':
      if (strcmp(name, "ul") == 0) return TAG_ListUnordered;
      return TAG_None;
    default:
      return TAG_None;
  }
}

bool ChapterHtmlSlimParser::asciiContainsInsensitive(const char* haystack, const char* needle) {
  if (!haystack || !needle || needle[0] == '\0') {
    return false;
  }

  for (const char* start = haystack; *start != '\0'; ++start) {
    size_t index = 0;
    while (needle[index] != '\0' && start[index] != '\0') {
      char hay = start[index];
      char nee = needle[index];
      if (hay >= 'A' && hay <= 'Z') hay = static_cast<char>(hay + ('a' - 'A'));
      if (nee >= 'A' && nee <= 'Z') nee = static_cast<char>(nee + ('a' - 'A'));
      if (hay != nee) {
        break;
      }
      ++index;
    }
    if (needle[index] == '\0') {
      return true;
    }
  }

  return false;
}

bool ChapterHtmlSlimParser::looksLikeCaptionTag(const char* name, const char* classAttr, const char* idAttr) {
  if (name && strcmp(name, "figcaption") == 0) {
    return true;
  }

  return asciiContainsInsensitive(classAttr, "caption") || asciiContainsInsensitive(classAttr, "figcaption") ||
         asciiContainsInsensitive(classAttr, "legend") || asciiContainsInsensitive(idAttr, "caption") ||
         asciiContainsInsensitive(idAttr, "figcaption") || asciiContainsInsensitive(idAttr, "legend");
}

bool ChapterHtmlSlimParser::looksLikeCaptionText(const ParsedText& text) const {
  if (text.size() == 0 || text.size() > 24) {
    return false;
  }

  const std::string preview = text.previewText(10, 128);
  if (preview.empty()) {
    return false;
  }

  static const char* kCaptionPrefixes[] = {
      "Рисунок",    "рисунок",     "Рис.",       "рис.",       "Figure",
      "figure",     "Fig.",        "fig.",       "Иллюстрация", "иллюстрация",
      "Схема",      "схема",
  };

  for (const char* prefix : kCaptionPrefixes) {
    const size_t prefixLen = strlen(prefix);
    if (preview.size() >= prefixLen && preview.compare(0, prefixLen, prefix) == 0) {
      return true;
    }
  }

  return false;
}

bool ChapterHtmlSlimParser::measureTextBlockLineCount(const ParsedText& text, size_t& lineCount) {
  lineCount = 0;
  ParsedText previewBlock = text;
  return previewBlock.layoutAndExtractLines(
      renderer, config.fontId, config.viewportWidth,
      [&lineCount](const std::shared_ptr<TextBlock>&) { ++lineCount; }, true,
      [this]() -> bool { return stopRequested_ || shouldAbort(); });
}

int ChapterHtmlSlimParser::paragraphSpacing() const {
  switch (config.spacingLevel) {
    case 1:
      return effectiveLineHeight_ / 4;
    case 3:
      return effectiveLineHeight_;
    default:
      return 0;
  }
}

void ChapterHtmlSlimParser::recomputeCurrentPageNextY() {
  if (!currentPage || currentPage->elements.empty()) {
    currentPageNextY = 0;
    return;
  }

  int16_t maxBottom = 0;
  for (const auto& element : currentPage->elements) {
    switch (element->getTag()) {
      case TAG_PageLine:
        maxBottom = std::max<int16_t>(maxBottom, element->yPos + effectiveLineHeight_);
        break;
      case TAG_PageImage: {
        const auto& image = static_cast<const PageImage&>(*element);
        maxBottom = std::max<int16_t>(maxBottom, element->yPos + image.getImageBlock().getHeight() + effectiveLineHeight_);
        break;
      }
    }
  }

  currentPageNextY = maxBottom;
}

bool ChapterHtmlSlimParser::completeCurrentPageAndStartFresh() {
  if (currentPage && !currentPage->elements.empty()) {
    ++pagesCreated_;
    if (!completePageFn(std::move(currentPage))) {
      stopRequested_ = true;
      if (xmlParser_) {
        XML_StopParser(xmlParser_, XML_TRUE);
      }
      return false;
    }
    parseStartTime_ = millis();
  }

  currentPage.reset(new Page());
  currentPageNextY = 0;
  return true;
}

bool ChapterHtmlSlimParser::placePendingImage() {
  if (!pendingPageImage_) {
    return true;
  }

  auto image = pendingPageImage_;
  pendingPageImage_.reset();
  LOG_DBG(TAG, "Replaying deferred image node=%s src=%s", image->getSourceNodeId().empty() ? "-" : image->getSourceNodeId().c_str(),
          image->getSourcePath().empty() ? "-" : image->getSourcePath().c_str());
  addImageToPage(image);
  if (stopRequested_ && !pendingPageImage_) {
    pendingPageImage_ = std::move(image);
  }
  return !stopRequested_;
}

void ChapterHtmlSlimParser::maybeKeepTrailingImageWithCaption(const bool likelyCaption) {
  if (stopRequested_ || !likelyCaption || !currentPage || !currentTextBlock || currentTextBlock->isEmpty() ||
      currentPage->elements.empty()) {
    return;
  }

  PageElement* lastElement = currentPage->elements.back().get();
  if (!lastElement || lastElement->getTag() != TAG_PageImage) {
    return;
  }

  size_t lineCount = 0;
  if (!measureTextBlockLineCount(*currentTextBlock, lineCount) || stopRequested_ || lineCount == 0) {
    return;
  }

  const auto& pageImage = static_cast<const PageImage&>(*lastElement);
  const int captionHeight = static_cast<int>(lineCount) * effectiveLineHeight_ + paragraphSpacing();
  if (currentPageNextY + captionHeight <= config.viewportHeight) {
    return;
  }

  const int requiredHeight = pageImage.getImageBlock().getHeight() + effectiveLineHeight_ + captionHeight;
  if (requiredHeight > config.viewportHeight) {
    return;
  }

  const std::shared_ptr<ImageBlock> imageBlock = pageImage.getImageBlockShared();
  const std::string& imageRef =
      imageBlock->getSourceNodeId().empty() ? imageBlock->getSourcePath() : imageBlock->getSourceNodeId();
  LOG_DBG(TAG, "Moving image to keep caption on same page: %s", imageRef.empty() ? "-" : imageRef.c_str());

  currentPage->elements.pop_back();
  recomputeCurrentPageNextY();

  if (!currentPage->elements.empty() && !completeCurrentPageAndStartFresh()) {
    pendingPageImage_ = imageBlock;
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  addImageToPage(imageBlock);
}

void ChapterHtmlSlimParser::maybeDeferTrailingTallImageText(const bool likelyCaption) {
  if (stopRequested_ || likelyCaption || !currentPage || !currentTextBlock || currentTextBlock->isEmpty() ||
      currentPage->elements.empty()) {
    return;
  }

  PageElement* lastElement = currentPage->elements.back().get();
  if (!lastElement || lastElement->getTag() != TAG_PageImage) {
    return;
  }

  const auto& pageImage = static_cast<const PageImage&>(*lastElement);
  if (pageImage.getImageBlock().getHeight() <= config.viewportHeight / 2) {
    return;
  }

  LOG_DBG(TAG, "Deferring text after tall image to next page");
  completeCurrentPageAndStartFresh();
}

enum class ImageInterruptReason : uint8_t {
  None,
  StopRequested,
  Timeout,
  LowMemory,
};

const char* readItemStatusToString(const ChapterHtmlSlimParser::ReadItemStatus status) {
  switch (status) {
    case ChapterHtmlSlimParser::ReadItemStatus::Success:
      return "success";
    case ChapterHtmlSlimParser::ReadItemStatus::Aborted:
      return "aborted";
    case ChapterHtmlSlimParser::ReadItemStatus::NotFound:
      return "not-found";
    case ChapterHtmlSlimParser::ReadItemStatus::ArchiveError:
      return "archive-error";
    case ChapterHtmlSlimParser::ReadItemStatus::IoError:
      return "io-error";
    case ChapterHtmlSlimParser::ReadItemStatus::WriteError:
      return "write-error";
  }
  return "unknown";
}

const char* cachedImageStatusToString(const ChapterHtmlSlimParser::CachedImageStatus status) {
  switch (status) {
    case ChapterHtmlSlimParser::CachedImageStatus::Success:
      return "success";
    case ChapterHtmlSlimParser::CachedImageStatus::RetryableInterruption:
      return "retryable-interruption";
    case ChapterHtmlSlimParser::CachedImageStatus::TerminalFailure:
      return "terminal-failure";
  }
  return "unknown";
}

const char* imageFailureClassToString(const ChapterHtmlSlimParser::ImageFailureClass failureClass) {
  switch (failureClass) {
    case ChapterHtmlSlimParser::ImageFailureClass::None:
      return "none";
    case ChapterHtmlSlimParser::ImageFailureClass::AbortRequested:
      return "abort-requested";
    case ChapterHtmlSlimParser::ImageFailureClass::Timeout:
      return "timeout";
    case ChapterHtmlSlimParser::ImageFailureClass::LowMemory:
      return "low-memory";
    case ChapterHtmlSlimParser::ImageFailureClass::DataUri:
      return "data-uri";
    case ChapterHtmlSlimParser::ImageFailureClass::UnsupportedFormat:
      return "unsupported-format";
    case ChapterHtmlSlimParser::ImageFailureClass::CacheHit:
      return "cache-hit";
    case ChapterHtmlSlimParser::ImageFailureClass::CachedOpenFailed:
      return "cached-open-failed";
    case ChapterHtmlSlimParser::ImageFailureClass::CachedBmpInvalid:
      return "cached-bmp-invalid";
    case ChapterHtmlSlimParser::ImageFailureClass::TempOpenFailed:
      return "temp-open-failed";
    case ChapterHtmlSlimParser::ImageFailureClass::ExtractAborted:
      return "extract-aborted";
    case ChapterHtmlSlimParser::ImageFailureClass::ExtractFailed:
      return "extract-failed";
    case ChapterHtmlSlimParser::ImageFailureClass::ConvertAborted:
      return "convert-aborted";
    case ChapterHtmlSlimParser::ImageFailureClass::ConvertFailed:
      return "convert-failed";
    case ChapterHtmlSlimParser::ImageFailureClass::GeneratedBmpInvalid:
      return "generated-bmp-invalid";
    case ChapterHtmlSlimParser::ImageFailureClass::MissingSrc:
      return "missing-src";
    case ChapterHtmlSlimParser::ImageFailureClass::ReadItemUnavailable:
      return "read-item-unavailable";
    case ChapterHtmlSlimParser::ImageFailureClass::ImageCacheDisabled:
      return "image-cache-disabled";
  }
  return "unknown";
}

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

// given the start and end of a tag, check to see if it matches a known tag
bool matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

void ChapterHtmlSlimParser::flushPartWordBuffer() {
  if (!currentTextBlock || partWordBufferIndex == 0) {
    partWordBufferIndex = 0;
    return;
  }

  // Determine font style from HTML tags and CSS
  const bool isBold = boldUntilDepth < depth || cssBoldUntilDepth < depth;
  const bool isItalic = italicUntilDepth < depth || cssItalicUntilDepth < depth;

  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold && isItalic) {
    fontStyle = EpdFontFamily::BOLD_ITALIC;
  } else if (isBold) {
    fontStyle = EpdFontFamily::BOLD;
  } else if (isItalic) {
    fontStyle = EpdFontFamily::ITALIC;
  }

  partWordBuffer[partWordBufferIndex] = '\0';
  partWordBufferIndex = utf8NormalizeNfc(partWordBuffer, partWordBufferIndex);
  currentTextBlock->addWord(partWordBuffer, fontStyle);
  partWordBufferIndex = 0;
}

void ChapterHtmlSlimParser::appendPartWordBytes(const char* data, int len) {
  int remaining = len;
  const char* src = data;

  while (remaining > 0) {
    if (partWordBufferIndex >= MAX_WORD_SIZE) {
      flushPartWordBuffer();
    }

    const int spaceLeft = MAX_WORD_SIZE - partWordBufferIndex;
    const int chunkLen = utf8SafePrefixLength(src, remaining, spaceLeft);
    memcpy(partWordBuffer + partWordBufferIndex, src, chunkLen);
    partWordBufferIndex += chunkLen;
    src += chunkLen;
    remaining -= chunkLen;

    if (partWordBufferIndex >= MAX_WORD_SIZE) {
      flushPartWordBuffer();
    }
  }
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const TextBlock::BLOCK_STYLE style, const bool likelyCaption) {
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      currentTextBlock->setStyle(style);
      currentTextBlock->setRtl(pendingRtl_);
      currentBlockLikelyCaption_ = likelyCaption;
      return;
    }

    makePages();
    pendingEmergencySplit_ = false;

    // If page batch limit was hit during makePages(), the text block may still
    // contain words that weren't laid out yet. Preserve them for resumeParsing()
    // to process before the XML parser continues.
    if (stopRequested_) {
      pendingNewTextBlock_ = true;
      pendingBlockStyle_ = style;
      pendingBlockLikelyCaption_ = likelyCaption;
      return;
    }
  }
  currentBlockLikelyCaption_ = likelyCaption;
  currentTextBlock.reset(new ParsedText(style, config.indentLevel, config.hyphenation, true, pendingRtl_));
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  (void)atts;
  const TagFlags tagFlags = classifyTag(name);

  // Prevent stack overflow from deeply nested XML
  if (self->depth >= MAX_XML_DEPTH) {
    XML_StopParser(self->xmlParser_, XML_FALSE);
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (hasTagFlag(tagFlags, TAG_Image)) {
    const char* srcAttr = nullptr;
    const char* altText = nullptr;
    const char* imageIdAttr = nullptr;
    const char* imageClassAttr = nullptr;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0 && atts[i + 1][0] != '\0') {
          srcAttr = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0 && atts[i + 1][0] != '\0') {
          altText = atts[i + 1];
        } else if (strcmp(atts[i], "id") == 0 && atts[i + 1][0] != '\0') {
          imageIdAttr = atts[i + 1];
        } else if (strcmp(atts[i], "class") == 0 && atts[i + 1][0] != '\0') {
          imageClassAttr = atts[i + 1];
        }
      }
    }

    const char* nodeId = imageIdAttr ? imageIdAttr : imageClassAttr;
    LOG_DBG(TAG, "Found image: node=%s src=%s", nodeId ? nodeId : "-", srcAttr ? srcAttr : "(empty)");

    // Decode IRI reference: strip query/fragment, percent-decode.
    // EPUB XHTML content uses IRI references (RFC 3987) that may contain
    // percent-encoded characters or fragment identifiers — these must be
    // resolved to a filesystem path before ZIP entry lookup.
    const std::string srcDecoded = srcAttr
        ? FsHelpers::percentDecode(FsHelpers::stripQueryAndFragment(srcAttr))
        : std::string();

    // Silently skip unsupported image formats (GIF, SVG, WebP, etc.)
    if (!srcDecoded.empty() && !ImageConverterFactory::isSupported(srcDecoded)) {
      LOG_DBG(TAG, "Skipping unsupported format: %s (raw: %s)", srcDecoded.c_str(), srcAttr);
      self->depth += 1;
      return;
    }

    // Image processing inside an Expat element callback must resolve to a
    // committed layout decision for this pass. Terminal image failures can
    // safely degrade to placeholder text, but retryable interruptions must not
    // be serialized into the section cache: otherwise a transient abort while
    // background parsing is in flight permanently turns the real image into
    // "[Image]" for later page loads. For retryable interruptions we therefore
    // stop before committing the current image-bearing pass and force a later
    // restart from the last durable cache boundary.
    std::string resolvedPath;
    std::string cachedPath;
    std::string fallbackReason = "image-pipeline-disabled";
    auto fallbackClass = ImageFailureClass::ImageCacheDisabled;
    bool cacheHit = false;
    if (!srcDecoded.empty() && self->readItemFn && !self->imageCachePath.empty()) {
      LOG_INF(TAG, "[CONTENT][IMAGE] node start node=%s src=%s decoded=%s resolved=%s quick=%u", nodeId ? nodeId : "-",
              srcAttr, srcDecoded.c_str(),
              FsHelpers::normalisePath(self->chapterBasePath + srcDecoded).c_str(),
              static_cast<unsigned>(self->quickImageDecode_));
      CachedImageResult cacheResult = self->cacheImage(srcDecoded);
      cachedPath = std::move(cacheResult.cachedBmpPath);
      resolvedPath = std::move(cacheResult.resolvedPath);
      fallbackReason = std::move(cacheResult.failureReason);
      fallbackClass = cacheResult.failureClass;
      cacheHit = cacheResult.cacheHit;
      if (self->fatalAbortRequested_) {
        self->depth += 1;
        return;
      }
      if (cacheResult.status == CachedImageStatus::RetryableInterruption) {
        LOG_ERR(
            TAG,
            "[CONTENT][IMAGE] interrupted node=%s src=%s resolved=%s cached=%s cacheHit=%u status=%s class=%s "
            "reason=%s retryable=%u action=suspend-restart",
            nodeId ? nodeId : "-", srcAttr, resolvedPath.empty() ? "-" : resolvedPath.c_str(),
            cachedPath.empty() ? "-" : cachedPath.c_str(), static_cast<unsigned>(cacheHit),
            cachedImageStatusToString(cacheResult.status), imageFailureClassToString(cacheResult.failureClass),
            fallbackReason.empty() ? "unknown" : fallbackReason.c_str(), static_cast<unsigned>(cacheResult.retryable));
        self->aborted_ = true;
        self->depth += 1;
        self->requestCooperativeSuspend();
        return;
      }
      if (cacheResult.success) {
        // Skip tiny decorative images (e.g. 1px-tall line separators) - invisible on e-paper
        if (cacheResult.width < 20 || cacheResult.height < 20) {
          LOG_INF(TAG, "[CONTENT][IMAGE] skip tiny node=%s src=%s resolved=%s cached=%s size=%ux%u",
                  nodeId ? nodeId : "-", srcAttr, resolvedPath.empty() ? "-" : resolvedPath.c_str(),
                  cachedPath.empty() ? "-" : cachedPath.c_str(), static_cast<unsigned>(cacheResult.width),
                  static_cast<unsigned>(cacheResult.height));
          self->depth += 1;
          return;
        }
        LOG_DBG(TAG, "Image loaded: %dx%d", cacheResult.width, cacheResult.height);
        auto imageBlock = std::make_shared<ImageBlock>(cachedPath, cacheResult.width, cacheResult.height,
                                                       nodeId ? nodeId : "", srcDecoded, resolvedPath);

        // Flush any pending text block before adding image
        if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
          self->makePages();
        }

        self->addImageToPage(imageBlock);
        self->depth += 1;
        return;
      }
    } else {
      if (srcDecoded.empty()) {
        fallbackReason = "missing-src";
        fallbackClass = ImageFailureClass::MissingSrc;
      } else if (!self->readItemFn) {
        fallbackReason = "read-item-unavailable";
        fallbackClass = ImageFailureClass::ReadItemUnavailable;
      } else if (self->imageCachePath.empty()) {
        fallbackReason = "image-cache-disabled";
        fallbackClass = ImageFailureClass::ImageCacheDisabled;
      }
      LOG_DBG(TAG, "Image skipped: src=%d, readItemFn=%d, imageCachePath=%d", !srcDecoded.empty(),
              self->readItemFn != nullptr, !self->imageCachePath.empty());
    }

    LOG_INF(TAG,
            "[CONTENT][IMAGE] fallback parser node=%s src=%s resolved=%s cached=%s cacheHit=%u status=%s class=%s "
            "reason=%s retryable=0 terminal=1 stage=placeholder-text",
            nodeId ? nodeId : "-", srcAttr ? srcAttr : "-", resolvedPath.empty() ? "-" : resolvedPath.c_str(),
            cachedPath.empty() ? "-" : cachedPath.c_str(), static_cast<unsigned>(cacheHit),
            cachedImageStatusToString(CachedImageStatus::TerminalFailure),
            imageFailureClassToString(fallbackClass),
            fallbackReason.empty() ? "unknown" : fallbackReason.c_str());

    // Fallback: show placeholder with alt text if image processing failed
    self->startNewTextBlock(TextBlock::CENTER_ALIGN);
    if (self->currentTextBlock) {
      if (altText) {
        std::string placeholder = std::string("[Image: ") + altText + "]";
        self->currentTextBlock->addWord(placeholder.c_str(), EpdFontFamily::ITALIC);
      } else {
        self->currentTextBlock->addWord("[Image]", EpdFontFamily::ITALIC);
      }
    }

    self->depth += 1;
    return;
  }

  // Special handling for tables - show placeholder text instead of dropping silently
  // TODO: Render tables - parse table structure (thead, tbody, tr, td, th), calculate
  // column widths, handle colspan/rowspan, and render as formatted text grid.
  if (strcmp(name, "table") == 0) {
    // For now, add placeholder text
    self->startNewTextBlock(TextBlock::CENTER_ALIGN);
    if (self->currentTextBlock) {
      self->currentTextBlock->addWord("[Table omitted]", EpdFontFamily::ITALIC);
    }

    // Skip table contents
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  if (hasTagFlag(tagFlags, TAG_Skip)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Skip empty anchor tags with aria-hidden (Pandoc line number anchors)
  // These appear as: <a href="#cb1-1" aria-hidden="true" tabindex="-1"></a>
  if (strcmp(name, "a") == 0 && atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "aria-hidden") == 0 && strcmp(atts[i + 1], "true") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Extract class, style, dir, and id attributes
  const char* classAttr = nullptr;
  const char* styleAttr = nullptr;
  const char* dirAttr = nullptr;
  const char* idAttr = nullptr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "dir") == 0) {
        dirAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0 && atts[i + 1][0] != '\0') {
        idAttr = atts[i + 1];
      }
    }
  }

  // Query CSS for combined style (tag + classes + inline)
  CssStyle cssStyle;
  if (self->cssParser_) {
    if (++self->elementCounter_ % CSS_HEAP_CHECK_INTERVAL == 0) {
      self->cssHeapOk_ = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= MIN_FREE_HEAP;
      if (!self->cssHeapOk_) {
        LOG_ERR(TAG, "Low memory, skipping CSS lookups");
      }
    }
    if (self->cssHeapOk_) {
      cssStyle = self->cssParser_->getCombinedStyle(name, classAttr ? classAttr : "");
    }
  }
  // Inline styles override stylesheet rules (static method, no instance needed)
  if (styleAttr && styleAttr[0] != '\0') {
    cssStyle.merge(CssParser::parseInlineStyle(styleAttr));
  }
  // HTML dir attribute overrides CSS direction (case-insensitive per HTML spec)
  if (dirAttr && strcasecmp(dirAttr, "rtl") == 0) {
    cssStyle.direction = TextDirection::Rtl;
    cssStyle.hasDirection = true;
  } else if (dirAttr && strcasecmp(dirAttr, "ltr") == 0) {
    cssStyle.direction = TextDirection::Ltr;
    cssStyle.hasDirection = true;
  }

  // Apply CSS font-weight and font-style
  if (cssStyle.hasFontWeight && cssStyle.fontWeight == CssFontWeight::Bold) {
    self->cssBoldUntilDepth = min(self->cssBoldUntilDepth, self->depth);
  }
  if (cssStyle.hasFontStyle && cssStyle.fontStyle == CssFontStyle::Italic) {
    self->cssItalicUntilDepth = min(self->cssItalicUntilDepth, self->depth);
  }

  // Track direction for next text block creation
  if (cssStyle.hasDirection) {
    self->pendingRtl_ = (cssStyle.direction == TextDirection::Rtl);
    self->rtlUntilDepth_ = min(self->rtlUntilDepth_, self->depth);
  }

  if (hasTagFlag(tagFlags, TAG_Header)) {
    self->startNewTextBlock(TextBlock::CENTER_ALIGN);
    self->alignStack_.push_back({self->depth, TextBlock::CENTER_ALIGN});
    self->boldUntilDepth = min(self->boldUntilDepth, self->depth);
  } else if (hasTagFlag(tagFlags, TAG_Block)) {
    if (hasTagFlag(tagFlags, TAG_LineBreak)) {
      self->flushPartWordBuffer();
      const auto style = self->currentTextBlock ? self->currentTextBlock->getStyle()
                                                : static_cast<TextBlock::BLOCK_STYLE>(self->config.paragraphAlignment);
      self->startNewTextBlock(style, self->currentBlockLikelyCaption_);
    } else {
      // Determine block style: CSS text-align takes precedence, then inheritance, then default
      TextBlock::BLOCK_STYLE blockStyle = static_cast<TextBlock::BLOCK_STYLE>(self->config.paragraphAlignment);
      bool hasExplicitAlign = false;
      if (cssStyle.hasTextAlign) {
        hasExplicitAlign = true;
        switch (cssStyle.textAlign) {
          case TextAlign::Left:
            blockStyle = TextBlock::LEFT_ALIGN;
            break;
          case TextAlign::Right:
            blockStyle = TextBlock::RIGHT_ALIGN;
            break;
          case TextAlign::Center:
            blockStyle = TextBlock::CENTER_ALIGN;
            break;
          case TextAlign::Justify:
            blockStyle = TextBlock::JUSTIFIED;
            break;
          default:
            hasExplicitAlign = false;
            break;
        }
      }
      // CSS text-align is inherited: use parent's alignment if no explicit value
      if (!hasExplicitAlign && !self->alignStack_.empty()) {
        blockStyle = self->alignStack_.back().style;
      }
      // Push to inheritance stack if this element sets an explicit alignment
      if (hasExplicitAlign) {
        self->alignStack_.push_back({self->depth, blockStyle});
      }
      self->startNewTextBlock(blockStyle, self->looksLikeCaptionTag(name, classAttr, idAttr));

      if (hasTagFlag(tagFlags, TAG_ListItem) && !self->listStack_.empty()) {
        auto& listEntry = self->listStack_.back();
        listEntry.counter++;
        char marker[12] = {};
        if (listEntry.isOrdered) {
          snprintf(marker, sizeof(marker), "%d.", listEntry.counter);
        } else {
          strcpy(marker, "\xe2\x80\xa2");  // U+2022 BULLET
        }
        if (self->currentTextBlock && !self->pendingNewTextBlock_) {
          self->currentTextBlock->addWord(marker, EpdFontFamily::REGULAR);
        } else {
          memcpy(self->pendingListMarker_, marker, sizeof(self->pendingListMarker_));
        }
      }
    }
  } else if (hasTagFlag(tagFlags, TAG_Bold)) {
    self->boldUntilDepth = min(self->boldUntilDepth, self->depth);
  } else if (hasTagFlag(tagFlags, TAG_Italic)) {
    self->italicUntilDepth = min(self->italicUntilDepth, self->depth);
  } else if (hasTagFlag(tagFlags, TAG_ListUnordered) || hasTagFlag(tagFlags, TAG_ListOrdered)) {
    self->listStack_.push_back({self->depth, hasTagFlag(tagFlags, TAG_ListOrdered), 0});
  }

  // Record anchor-to-page mapping (after block handling so pagesCreated_ reflects current page)
  if (idAttr && idAttr[0] != '\0') {
    self->anchorMap_.emplace_back(idAttr, self->pagesCreated_);
  }

  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
  const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
  const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
  const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

  int offset = 0;
  while (offset < len) {
    while (offset < len && isWhitespace(s[offset])) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      offset++;
    }

    if (offset + 2 < len && s[offset] == FEFF_BYTE_1 && s[offset + 1] == FEFF_BYTE_2 && s[offset + 2] == FEFF_BYTE_3) {
      offset += 3;
      continue;
    }

    const int runStart = offset;
    while (offset < len && !isWhitespace(s[offset])) {
      if (offset + 2 < len && s[offset] == FEFF_BYTE_1 && s[offset + 1] == FEFF_BYTE_2 && s[offset + 2] == FEFF_BYTE_3) {
        break;
      }
      offset++;
    }

    if (offset > runStart) {
      self->appendPartWordBytes(s + runStart, offset - runStart);
    }

    if (offset + 2 < len && s[offset] == FEFF_BYTE_1 && s[offset + 1] == FEFF_BYTE_2 && s[offset + 2] == FEFF_BYTE_3) {
      offset += 3;
    }
  }

  // Flag for deferred split - handled outside XML callback to avoid stack overflow
  if (self->currentTextBlock && self->currentTextBlock->size() > 750) {
    self->pendingEmergencySplit_ = true;
  }
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  const TagFlags tagFlags = classifyTag(name);

  if (self->partWordBufferIndex > 0) {
    // Only flush out part word buffer if we're closing a block tag or are at the top of the HTML file.
    // We don't want to flush out content when closing inline tags like <span>.
    // Currently this also flushes out on closing <b> and <i> tags, but they are line tags so that shouldn't happen,
    // text styling needs to be overhauled to fix it.
    const bool shouldBreakText = hasTagFlag(tagFlags, TAG_Block) || hasTagFlag(tagFlags, TAG_Header) ||
                                 hasTagFlag(tagFlags, TAG_Bold) || hasTagFlag(tagFlags, TAG_Italic) ||
                                 self->depth == 1;

    if (shouldBreakText) {
      self->flushPartWordBuffer();
    }
  }

  self->depth -= 1;

  const bool headerOrBlockTag = hasTagFlag(tagFlags, TAG_Header) || hasTagFlag(tagFlags, TAG_Block);

  if (headerOrBlockTag && self->currentTextBlock && self->currentTextBlock->isEmpty()) {
    self->currentTextBlock->setStyle(static_cast<TextBlock::BLOCK_STYLE>(self->config.paragraphAlignment));
  }

  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }
  if (self->cssBoldUntilDepth == self->depth) {
    self->cssBoldUntilDepth = INT_MAX;
  }
  if (self->cssItalicUntilDepth == self->depth) {
    self->cssItalicUntilDepth = INT_MAX;
  }
  if (self->rtlUntilDepth_ == self->depth) {
    self->rtlUntilDepth_ = INT_MAX;
    self->pendingRtl_ = false;
  }
  while (!self->alignStack_.empty() && self->alignStack_.back().depth >= self->depth) {
    self->alignStack_.pop_back();
  }
  while (!self->listStack_.empty() && self->listStack_.back().depth >= self->depth) {
    self->listStack_.pop_back();
  }
}

void XMLCALL ChapterHtmlSlimParser::defaultHandler(void* userData, const XML_Char* s, int len) {
  // Called for text that expat doesn't handle — primarily undeclared entities.
  // Expat handles the 5 built-in XML entities (&amp; &lt; &gt; &quot; &apos;) and any
  // entities declared in the document's DTD. This catches HTML entities like &nbsp;,
  // &mdash;, &ldquo; etc. that many EPUBs use without proper DTD declarations.
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8 = lookupHtmlEntity(s + 1, len - 2);
    if (utf8) {
      characterData(userData, utf8, static_cast<int>(strlen(utf8)));
      return;
    }
  }
  // Not a recognized entity — silently drop.
  // The default handler also receives XML/DOCTYPE declarations,
  // comments, and processing instructions which must not become visible text.
}

bool ChapterHtmlSlimParser::shouldAbort() {
  if (cooperativeAbortRequested_ || fatalAbortRequested_) {
    return true;
  }

  // Check external abort callback first (cooperative cancellation)
  if (externalAbortCallback_ && externalAbortCallback_()) {
    cooperativeAbortRequested_ = true;
    LOG_DBG(TAG, "External abort requested");
    return true;
  }

  // Check timeout
  if (millis() - parseStartTime_ > MAX_PARSE_TIME_MS) {
    fatalAbortRequested_ = true;
    LOG_ERR(TAG, "Parse timeout exceeded (%u ms)", MAX_PARSE_TIME_MS);
    return true;
  }

  // Check memory pressure
  const size_t freeHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeHeap < MIN_FREE_HEAP) {
    fatalAbortRequested_ = true;
    LOG_ERR(TAG, "Low memory (%zu bytes free)", freeHeap);
    return true;
  }

  return false;
}

void ChapterHtmlSlimParser::requestCooperativeSuspend() {
  stopRequested_ = true;
  if (xmlParser_) {
    XML_StopParser(xmlParser_, XML_TRUE);
  }
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() { cleanupParser(); }

void ChapterHtmlSlimParser::cleanupParser() {
  if (xmlParser_) {
    XML_SetElementHandler(xmlParser_, nullptr, nullptr);
    XML_SetCharacterDataHandler(xmlParser_, nullptr);
    XML_SetDefaultHandlerExpand(xmlParser_, nullptr);
    XML_ParserFree(xmlParser_);
    xmlParser_ = nullptr;
  }
  if (file_) {
    file_.close();
  }
  currentPage.reset();
  currentTextBlock.reset();
  pendingPageImage_.reset();
  cooperativeAbortRequested_ = false;
  fatalAbortRequested_ = false;
  suspended_ = false;
  xmlDone_ = false;
}

bool ChapterHtmlSlimParser::initParser() {
  parseStartTime_ = millis();
  loopCounter_ = 0;
  elementCounter_ = 0;
  effectiveLineHeight_ = static_cast<int>(renderer.getEffectiveLineHeight(config.fontId) * config.lineCompression);
  cssHeapOk_ = true;
  pendingEmergencySplit_ = false;
  pendingNewTextBlock_ = false;
  pendingBlockLikelyCaption_ = false;
  currentBlockLikelyCaption_ = false;
  aborted_ = false;
  cooperativeAbortRequested_ = false;
  fatalAbortRequested_ = false;
  stopRequested_ = false;
  suspended_ = false;
  xmlDone_ = false;
  alignStack_.clear();
  alignStack_.reserve(8);
  listStack_.clear();
  listStack_.reserve(4);
  anchorMap_.clear();
  pendingListMarker_[0] = '\0';
  pendingPageImage_.reset();
  dataUriStripper_.reset();
  startNewTextBlock(static_cast<TextBlock::BLOCK_STYLE>(config.paragraphAlignment));

  xmlParser_ = XML_ParserCreate(nullptr);
  if (!xmlParser_) {
    LOG_ERR(TAG, "Couldn't allocate memory for parser");
    return false;
  }

  {
    snapix::spi::SharedBusLock busLock;
    if (!SdMan.openFileForRead("EHP", filepath, file_)) {
      XML_ParserFree(xmlParser_);
      xmlParser_ = nullptr;
      return false;
    }
    totalSize_ = file_.size();
  }
  bytesRead_ = 0;
  lastProgress_ = -1;
  pagesCreated_ = 0;

  // Allow parsing documents with undeclared HTML entities (e.g. &nbsp;, &mdash;).
  // Without this, Expat returns XML_ERROR_UNDEFINED_ENTITY for any entity
  // not declared in the document's DTD. UseForeignDTD makes Expat treat
  // undeclared entities as "skipped" rather than errors, passing them to
  // our default handler for resolution via the HTML entity lookup table.
  XML_UseForeignDTD(xmlParser_, XML_TRUE);

  XML_SetUserData(xmlParser_, this);
  XML_SetElementHandler(xmlParser_, startElement, endElement);
  XML_SetCharacterDataHandler(xmlParser_, characterData);
  XML_SetDefaultHandlerExpand(xmlParser_, defaultHandler);

  return true;
}

bool ChapterHtmlSlimParser::parseLoop() {
  int done;

  do {
    // Periodic safety check and yield
    if (++loopCounter_ % YIELD_CHECK_INTERVAL == 0) {
      if (shouldAbort()) {
        LOG_DBG(TAG, "Aborting parse, pages created: %u", pagesCreated_);
        if (fatalAbortRequested_) {
          aborted_ = true;
          break;
        }
        stopRequested_ = true;
        suspended_ = true;
        file_.close();
        return true;
      }
      vTaskDelay(1);  // Yield to prevent watchdog reset
    }

    constexpr size_t kReadChunkSize = 2048;
    constexpr size_t kDataUriPrefixSize = 10;  // max partial saved by DataUriStripper: "src=\"data:"
    void* const buf = XML_GetBuffer(xmlParser_, kReadChunkSize + kDataUriPrefixSize);
    if (!buf) {
      LOG_ERR(TAG, "Couldn't allocate memory for buffer");
      cleanupParser();
      return false;
    }

    int readResult;
    {
      snapix::spi::SharedBusLock busLock;
      readResult = file_.read(static_cast<uint8_t*>(buf), kReadChunkSize);
    }

    if (readResult <= 0) {
      LOG_ERR(TAG, "File read error");
      cleanupParser();
      return false;
    }

    // Strip data URIs BEFORE expat parses the buffer to prevent OOM on large embedded images.
    // This replaces src="data:image/..." with src="#" so expat never sees the huge base64 string.
    size_t len = static_cast<size_t>(readResult);
    const size_t originalLen = len;
    len = dataUriStripper_.strip(static_cast<char*>(buf), len, kReadChunkSize + kDataUriPrefixSize);

    // Update progress (call every 10% change to avoid too frequent updates)
    // Only show progress for larger chapters where rendering overhead is worth it
    bytesRead_ += originalLen;
    if (progressFn && totalSize_ >= MIN_SIZE_FOR_PROGRESS) {
      const int progress = static_cast<int>((bytesRead_ * 100) / totalSize_);
      if (lastProgress_ / 10 != progress / 10) {
        lastProgress_ = progress;
        progressFn(progress);
      }
    }

    done = file_.available() == 0;

    const auto status = XML_ParseBuffer(xmlParser_, static_cast<int>(len), done);
    if (status == XML_STATUS_ERROR) {
      LOG_ERR(TAG, "Parse error at line %lu: %s", XML_GetCurrentLineNumber(xmlParser_),
              XML_ErrorString(XML_GetErrorCode(xmlParser_)));
      cleanupParser();
      return false;
    }

    // XML_STATUS_SUSPENDED means completePageFn returned false (maxPages hit).
    // Parser state is preserved for resume. Close file to free handle.
    if (status == XML_STATUS_SUSPENDED) {
      suspended_ = true;
      file_.close();
      return true;
    }

    // Deferred emergency split - runs outside XML callback to avoid stack overflow.
    // Inside characterData(), the call chain includes expat internal frames (~1-2KB).
    // By splitting here, we save that stack space - critical for external fonts which
    // add extra frames through getExternalGlyphWidth() → ExternalFont::getGlyph() (SD I/O).
    if (pendingEmergencySplit_ && currentTextBlock && !currentTextBlock->isEmpty()) {
      pendingEmergencySplit_ = false;
      const size_t freeHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      if (freeHeap < MIN_LAYOUT_FREE_HEAP) {
        LOG_ERR(TAG, "Low memory (%zu), aborting parse", freeHeap);
        aborted_ = true;
        break;
      }
      LOG_DBG(TAG, "Text block too long (%zu words), splitting", currentTextBlock->size());
      currentTextBlock->setUseGreedyBreaking(true);
      currentTextBlock->layoutAndExtractLines(
          renderer, config.fontId, config.viewportWidth,
          [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); }, true,
          [this]() -> bool { return stopRequested_ || shouldAbort(); });
    }
  } while (!done);

  // Reached end of file or aborted — finalize
  // Process last page if there is still text
  if (currentTextBlock && !stopRequested_ && !aborted_) {
    makePages();
    if (stopRequested_) {
      // Batch limit hit while flushing final content — stay suspended
      suspended_ = true;
      xmlDone_ = true;
      file_.close();
      return true;
    }
    if (currentPage) {
      completePageFn(std::move(currentPage));
    }
    currentPage.reset();
    currentTextBlock.reset();
  }

  cleanupParser();
  return true;
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  if (!initParser()) {
    return false;
  }
  return parseLoop();
}

bool ChapterHtmlSlimParser::resumeParsing() {
  if (!suspended_ || !xmlParser_) {
    return false;
  }

  // XML parser already finished — just flush remaining content from finalization
  if (xmlDone_) {
    cooperativeAbortRequested_ = false;
    fatalAbortRequested_ = false;
    stopRequested_ = false;
    suspended_ = false;
    parseStartTime_ = millis();

    if (!placePendingImage()) {
      suspended_ = true;
      return true;
    }

    if (currentTextBlock && !currentTextBlock->isEmpty()) {
      makePages();
      if (stopRequested_) {
        // Still more content than fits in one batch — stay suspended
        suspended_ = true;
        return true;
      }
    }

    if (currentPage) {
      completePageFn(std::move(currentPage));
    }
    cleanupParser();
    return true;
  }

  // Reopen file at saved position (closed on suspend to free file handle)
  {
    snapix::spi::SharedBusLock busLock;
    if (!SdMan.openFileForRead("EHP", filepath, file_)) {
      LOG_ERR(TAG, "Failed to reopen file for resume");
      cleanupParser();
      return false;
    }
    file_.seek(bytesRead_);
  }

  // Reset per-extend state
  parseStartTime_ = millis();
  loopCounter_ = 0;
  elementCounter_ = 0;
  cooperativeAbortRequested_ = false;
  fatalAbortRequested_ = false;
  stopRequested_ = false;
  suspended_ = false;

  if (!placePendingImage()) {
    suspended_ = true;
    file_.close();
    return true;
  }

  // Lay out remaining words from the text block that was interrupted when the
  // previous batch hit its page limit. Without this, words after the page-break
  // point in a paragraph are silently lost.
  if (currentTextBlock && !currentTextBlock->isEmpty()) {
    makePages();
    if (stopRequested_) {
      // Remaining words filled another batch — stay suspended
      suspended_ = true;
      file_.close();
      return true;
    }
  }

  // Complete the deferred startNewTextBlock() that was interrupted by the batch limit.
  // The XML parser already processed startElement() for the new block tag, so we must
  // create the text block here before resuming — otherwise the new paragraph's text
  // would be appended to the old (now empty) text block with wrong style/no break.
  if (pendingNewTextBlock_) {
    pendingNewTextBlock_ = false;
    currentBlockLikelyCaption_ = pendingBlockLikelyCaption_;
    currentTextBlock.reset(
        new ParsedText(pendingBlockStyle_, config.indentLevel, config.hyphenation, true, pendingRtl_));
    if (pendingListMarker_[0] != '\0') {
      currentTextBlock->addWord(pendingListMarker_, EpdFontFamily::REGULAR);
      pendingListMarker_[0] = '\0';
    }
  }

  const auto status = XML_ResumeParser(xmlParser_);
  if (status == XML_STATUS_ERROR) {
    LOG_ERR(TAG, "Resume error: %s", XML_ErrorString(XML_GetErrorCode(xmlParser_)));
    cleanupParser();
    return false;
  }

  // If resume itself caused a suspend (maxPages hit again immediately), we're done.
  // Close file to free handle (same as the suspend path inside parseLoop).
  if (status == XML_STATUS_SUSPENDED) {
    suspended_ = true;
    file_.close();
    return true;
  }

  // If the file was already fully read before suspension (small file consumed in one
  // parseLoop iteration), the parser has now finished processing all buffered data.
  // Skip parseLoop() — calling XML_GetBuffer on a finalized parser returns NULL.
  if (file_.available() == 0) {
    if (currentTextBlock && !stopRequested_ && !aborted_) {
      makePages();
      if (stopRequested_) {
        suspended_ = true;
        xmlDone_ = true;
        file_.close();
        return true;
      }
      if (currentPage) {
        completePageFn(std::move(currentPage));
      }
      currentPage.reset();
      currentTextBlock.reset();
    }
    cleanupParser();
    return true;
  }

  // Continue the file-reading loop
  return parseLoop();
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  if (stopRequested_) return;

  if (currentPageNextY + effectiveLineHeight_ > config.viewportHeight) {
    ++pagesCreated_;
    if (!completePageFn(std::move(currentPage))) {
      // Preserve this line for the next batch — it's already been
      // extracted from the text block and would be lost otherwise
      currentPage.reset(new Page());
      currentPageNextY = 0;
      currentPage->elements.push_back(std::make_unique<PageLine>(std::move(line), 0, currentPageNextY));
      currentPageNextY += effectiveLineHeight_;
      stopRequested_ = true;
      if (xmlParser_) {
        XML_StopParser(xmlParser_, XML_TRUE);  // Resumable suspend
      }
      return;
    }
    parseStartTime_ = millis();
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  currentPage->elements.push_back(std::make_unique<PageLine>(std::move(line), 0, currentPageNextY));
  currentPageNextY += effectiveLineHeight_;
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    LOG_ERR(TAG, "No text block to make pages for");
    return;
  }

  flushPartWordBuffer();

  // Check memory before expensive layout operation
  const size_t freeHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeHeap < MIN_LAYOUT_FREE_HEAP) {
    LOG_ERR(TAG, "Insufficient memory for layout (%zu bytes)", freeHeap);
    currentTextBlock.reset();
    aborted_ = true;
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const bool likelyCaption = currentBlockLikelyCaption_ || looksLikeCaptionText(*currentTextBlock);
  maybeKeepTrailingImageWithCaption(likelyCaption);
  if (stopRequested_) {
    return;
  }
  maybeDeferTrailingTallImageText(likelyCaption);
  if (stopRequested_) {
    return;
  }

  uint16_t layoutCheckCounter = 0;
  const bool layoutCompleted = currentTextBlock->layoutAndExtractLines(
      renderer, config.fontId, config.viewportWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); }, true,
      [this, &layoutCheckCounter]() -> bool {
        if (stopRequested_) {
          return true;
        }

        if ((++layoutCheckCounter % 16) == 0) {
          vTaskDelay(1);  // Keep the watchdog happy during huge paragraph layout.
        }

        if (shouldAbort()) {
          stopRequested_ = true;
          if (fatalAbortRequested_) {
            aborted_ = true;
          }
          if (xmlParser_) {
            XML_StopParser(xmlParser_, XML_TRUE);  // Resumable suspend / cooperative abort.
          }
          return true;
        }
        return false;
      });

  if (!layoutCompleted) {
    return;
  }

  // Extra paragraph spacing based on spacingLevel (0=none, 1=small, 3=large)
  // Skip if aborted mid-block — spacing between paragraphs, not mid-paragraph
  if (!stopRequested_) {
    switch (config.spacingLevel) {
      case 1:
        currentPageNextY += effectiveLineHeight_ / 4;  // Small (1/4 line)
        break;
      case 3:
        currentPageNextY += effectiveLineHeight_;  // Large (full line)
        break;
    }
  }
}

bool ChapterHtmlSlimParser::validateCachedBmp(const std::string& cachedBmpPath, uint16_t& width, uint16_t& height,
                                              std::string& failureReason) {
  snapix::spi::SharedBusLock busLock;
  if (!busLock) {
    failureReason = "shared-bus-lock-unavailable";
    return false;
  }

  FsFile bmpFile;
  if (!SdMan.openFileForRead("EHP", cachedBmpPath, bmpFile)) {
    failureReason = "cached-open-failed";
    return false;
  }

  Bitmap bitmap(bmpFile, false);
  const BmpReaderError err = bitmap.parseHeaders();
  if (err != BmpReaderError::Ok) {
    failureReason = std::string("cached-bmp-parse-failed:") + Bitmap::errorToString(err);
    bmpFile.close();
    return false;
  }

  width = static_cast<uint16_t>(bitmap.getWidth());
  height = static_cast<uint16_t>(bitmap.getHeight());
  bmpFile.close();
  failureReason.clear();
  return true;
}

ChapterHtmlSlimParser::CachedImageResult ChapterHtmlSlimParser::cacheImage(const std::string& src) {
  CachedImageResult result;
  result.resolvedPath = FsHelpers::normalisePath(chapterBasePath + src);
  result.status = CachedImageStatus::TerminalFailure;
  LOG_INF(TAG, "[CONTENT][IMAGE] start src=%s resolved=%s quick=%u", src.c_str(), result.resolvedPath.c_str(),
          static_cast<unsigned>(quickImageDecode_));
  const uint32_t imageStartedMs = millis();
  const uint32_t imageTimeoutMs =
      quickImageDecode_ ? MAX_QUICK_IMAGE_PROCESS_TIME_MS : MAX_FULL_IMAGE_PROCESS_TIME_MS;
  ImageInterruptReason lastSeenInterrupt = ImageInterruptReason::None;
  auto classifyImageInterrupt = [this, imageStartedMs, imageTimeoutMs,
                                 &lastSeenInterrupt]() -> ImageInterruptReason {
    if (stopRequested_ || cooperativeAbortRequested_) {
      return ImageInterruptReason::StopRequested;
    }
    if (externalAbortCallback_ && externalAbortCallback_()) {
      return ImageInterruptReason::StopRequested;
    }
    if (millis() - imageStartedMs > imageTimeoutMs) {
      LOG_ERR(TAG, "Image processing timeout after %lu ms", static_cast<unsigned long>(imageTimeoutMs));
      return ImageInterruptReason::Timeout;
    }
    const size_t freeHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (freeHeap < MIN_IMAGE_PROCESS_FREE_HEAP) {
      LOG_ERR(TAG, "Low memory during image processing (%zu bytes free)", freeHeap);
      lastSeenInterrupt = ImageInterruptReason::LowMemory;
      return ImageInterruptReason::LowMemory;
    }
    return ImageInterruptReason::None;
  };
  auto setRetryable = [&](const char* reason, const ImageFailureClass failureClass) {
    result.failureReason = reason;
    result.failureClass = failureClass;
    result.retryable = true;
    result.status = CachedImageStatus::RetryableInterruption;
    result.success = false;
  };
  auto setTerminal = [&](const char* reason, const ImageFailureClass failureClass) {
    result.failureReason = reason;
    result.failureClass = failureClass;
    result.retryable = false;
    result.status = CachedImageStatus::TerminalFailure;
    result.success = false;
  };
  auto interruptToFailureClass = [](const ImageInterruptReason reason,
                                    const ImageFailureClass stopClass) -> ImageFailureClass {
    switch (reason) {
      case ImageInterruptReason::StopRequested:
        return stopClass;
      case ImageInterruptReason::Timeout:
        return ImageFailureClass::Timeout;
      case ImageInterruptReason::LowMemory:
        return ImageFailureClass::LowMemory;
      case ImageInterruptReason::None:
        break;
    }
    return stopClass;
  };
  auto interruptToReason = [](const ImageInterruptReason reason, const char* stopReason) -> const char* {
    switch (reason) {
      case ImageInterruptReason::StopRequested:
        return stopReason;
      case ImageInterruptReason::Timeout:
        return "timeout";
      case ImageInterruptReason::LowMemory:
        return "low-memory";
      case ImageInterruptReason::None:
        break;
    }
    return stopReason;
  };
  auto shouldAbortImage = [&]() -> bool {
    return classifyImageInterrupt() != ImageInterruptReason::None;
  };

  // Check abort before starting image processing
  if (const ImageInterruptReason interrupt = classifyImageInterrupt(); interrupt != ImageInterruptReason::None) {
    LOG_DBG(TAG, "Retryable image interruption before start");
    setRetryable(interruptToReason(interrupt, "abort-before-start"),
                 interruptToFailureClass(interrupt, ImageFailureClass::AbortRequested));
    return result;
  }

  // Skip data URIs - embedded base64 images can't be extracted and waste memory
  if (src.length() >= 5 && strncasecmp(src.c_str(), "data:", 5) == 0) {
    LOG_DBG(TAG, "Skipping embedded data URI image");
    setTerminal("data-uri", ImageFailureClass::DataUri);
    return result;
  }

  // Generate cache filename from hash
  size_t srcHash = std::hash<std::string>{}(result.resolvedPath);
  result.cachedBmpPath = imageCachePath + "/" + std::to_string(srcHash) + ".bmp";

  // Session blacklist: image already failed with timeout/abort this boot.
  // Skip immediately to break the infinite-retry loop in cold-extend.
  auto& failedHashes = sessionFailedImageHashes();
  if (failedHashes.count(srcHash)) {
    LOG_INF(TAG, "[CONTENT][IMAGE] session-blacklisted src=%s resolved=%s (skipped)", src.c_str(),
            result.resolvedPath.c_str());
    setTerminal("session-blacklisted", ImageFailureClass::ConvertFailed);
    return result;
  }

  std::string failedMarker = imageCachePath + "/" + std::to_string(srcHash) + ".failed";
  uint16_t cachedWidth = 0;
  uint16_t cachedHeight = 0;
  std::string cacheFailure;
  std::string tempPath;

  // ── Phase 1: cache check + extraction (holds SPI lock) ─────────────
  // The lock is released before conversion so that convertToBmp() never
  // runs at recursive-mutex depth > 1.  Deep nesting (depth 3) triggered
  // xTaskPriorityDisinherit asserts on single-core ESP32-C3.
  {
    snapix::spi::SharedBusLock preLock;
    if (!preLock) {
      setRetryable("shared-bus-lock-unavailable", ImageFailureClass::AbortRequested);
      return result;
    }

    // Check if already cached and validate the cached BMP before trusting it.
    if (SdMan.exists(result.cachedBmpPath.c_str())) {
      result.cacheHit = true;
      if (validateCachedBmp(result.cachedBmpPath, cachedWidth, cachedHeight, cacheFailure)) {
        consecutiveImageFailures_ = 0;
        result.width = cachedWidth;
        result.height = cachedHeight;
        result.success = true;
        result.retryable = false;
        result.status = CachedImageStatus::Success;
        result.failureClass = ImageFailureClass::CacheHit;
        result.failureReason = "cache-hit";
        LOG_INF(TAG, "[CONTENT][IMAGE] cache hit src=%s resolved=%s cached=%s size=%ux%u", src.c_str(),
                result.resolvedPath.c_str(), result.cachedBmpPath.c_str(), static_cast<unsigned>(result.width),
                static_cast<unsigned>(result.height));
        return result;
      }

      LOG_INF(TAG, "[CONTENT][IMAGE] cache invalid src=%s resolved=%s cached=%s reason=%s action=rebuild", src.c_str(),
              result.resolvedPath.c_str(), result.cachedBmpPath.c_str(), cacheFailure.c_str());
      SdMan.remove(result.cachedBmpPath.c_str());
      SdMan.remove(failedMarker.c_str());
    }

    // Failed markers are only trusted for extraction/format failures. Conversion
    // can still succeed later in quick mode, so allow supported images to retry.
    if (SdMan.exists(failedMarker.c_str())) {
      if (!ImageConverterFactory::isSupported(src)) {
        consecutiveImageFailures_++;
        setTerminal("failed-marker-unsupported-format", ImageFailureClass::UnsupportedFormat);
        return result;
      }
      SdMan.remove(failedMarker.c_str());
    }

    // Check if format is supported
    if (!ImageConverterFactory::isSupported(src)) {
      LOG_DBG(TAG, "Unsupported image format: %s", src.c_str());
      FsFile marker;
      if (SdMan.openFileForWrite("EHP", failedMarker, marker)) {
        marker.close();
      }
      consecutiveImageFailures_++;
      setTerminal("unsupported-format", ImageFailureClass::UnsupportedFormat);
      return result;
    }

    // Extract image to temp file (include hash in name for uniqueness)
    const std::string tempExt = FsHelpers::isPngFile(src) ? ".png" : ".jpg";
    tempPath = imageCachePath + "/.tmp_" + std::to_string(srcHash) + tempExt;
    FsFile tempFile;
    if (!SdMan.openFileForWrite("EHP", tempPath, tempFile)) {
      LOG_ERR(TAG, "Failed to create temp file for image");
      setRetryable("temp-open-failed", ImageFailureClass::TempOpenFailed);
      return result;
    }

    const ReadItemStatus readStatus = readItemFn(result.resolvedPath, tempFile, 1024, shouldAbortImage);
    if (readStatus != ReadItemStatus::Success) {
      LOG_INF(TAG, "[CONTENT][IMAGE] extract result src=%s resolved=%s result=%s", src.c_str(),
              result.resolvedPath.c_str(), readItemStatusToString(readStatus));
      tempFile.close();
      SdMan.remove(tempPath.c_str());
      if (readStatus == ReadItemStatus::Aborted) {
        setRetryable("extract-aborted", ImageFailureClass::ExtractAborted);
        return result;
      }
      if (readStatus == ReadItemStatus::NotFound) {
        FsFile marker;
        if (SdMan.openFileForWrite("EHP", failedMarker, marker)) {
          marker.close();
        }
        consecutiveImageFailures_++;
        setTerminal("extract-not-found", ImageFailureClass::ExtractFailed);
        return result;
      }
      if (readStatus == ReadItemStatus::ArchiveError) {
        FsFile marker;
        if (SdMan.openFileForWrite("EHP", failedMarker, marker)) {
          marker.close();
        }
        consecutiveImageFailures_++;
        setTerminal("extract-archive-error", ImageFailureClass::ExtractFailed);
        return result;
      }
      setRetryable(readStatus == ReadItemStatus::WriteError ? "extract-write-error" : "extract-io-error",
                   ImageFailureClass::ExtractFailed);
      return result;
    }
    tempFile.close();
  }  // preLock released — SPI bus free for convertToBmp

  // ── Phase 2: conversion (convertToBmp acquires its own lock) ───────
  const int maxImageHeight = config.viewportHeight;
  const int maxImageWidth = static_cast<int>(config.viewportWidth);
  auto tryConvert = [&](int maxWidth, int maxHeight, bool quickMode) -> bool {
    ImageConvertConfig convertConfig;
    convertConfig.maxWidth = maxWidth;
    convertConfig.maxHeight = maxHeight;
    convertConfig.quickMode = quickMode;
    convertConfig.logTag = "EHP";
    convertConfig.shouldAbort = shouldAbortImage;
    return ImageConverterFactory::convertToBmp(tempPath, result.cachedBmpPath, convertConfig);
  };

  bool success = tryConvert(maxImageWidth, maxImageHeight, quickImageDecode_);
  if (!success && !shouldAbortImage() && !quickImageDecode_) {
    LOG_INF(TAG, "[CONTENT][IMAGE] retry quick src=%s", result.resolvedPath.c_str());
    { snapix::spi::SharedBusLock lk; SdMan.remove(result.cachedBmpPath.c_str()); }
    success = tryConvert(maxImageWidth, maxImageHeight, true);
  }
  if (!success && !shouldAbortImage()) {
    const int fallbackWidth = std::max(64, (maxImageWidth * 3) / 4);
    const int fallbackHeight = std::max(64, (maxImageHeight * 3) / 4);
    if (fallbackWidth != maxImageWidth || fallbackHeight != maxImageHeight) {
      LOG_INF(TAG, "[CONTENT][IMAGE] retry reduced src=%s size=%dx%d", result.resolvedPath.c_str(), fallbackWidth,
              fallbackHeight);
      { snapix::spi::SharedBusLock lk; SdMan.remove(result.cachedBmpPath.c_str()); }
      success = tryConvert(fallbackWidth, fallbackHeight, true);
    }
  }

  // ── Phase 3: post-conversion cleanup (holds SPI lock) ─────────────
  {
    snapix::spi::SharedBusLock postLock;
    SdMan.remove(tempPath.c_str());

    if (!success) {
      const ImageInterruptReason interrupt = classifyImageInterrupt();
      // Use lastSeenInterrupt when current check shows none — transient conditions
      // like low memory resolve after the converter frees its buffers.
      const ImageInterruptReason effectiveInterrupt =
          (interrupt != ImageInterruptReason::None) ? interrupt : lastSeenInterrupt;
      LOG_ERR(TAG, "[CONTENT][IMAGE] convert failed: %s interrupt=%s (last=%s)", result.resolvedPath.c_str(),
              interruptToReason(interrupt, "none"), interruptToReason(effectiveInterrupt, "none"));
      SdMan.remove(result.cachedBmpPath.c_str());
      if (effectiveInterrupt != ImageInterruptReason::None) {
        // Session-blacklist images that timed out or hit OOM during conversion —
        // they're guaranteed to fail again on background cold-extend retries.
        // User-aborted (StopRequested) is NOT blacklisted: user may navigate
        // back and want the image rendered properly.
        if (effectiveInterrupt == ImageInterruptReason::Timeout ||
            effectiveInterrupt == ImageInterruptReason::LowMemory) {
          sessionFailedImageHashes().insert(srcHash);
          LOG_INF(TAG, "[CONTENT][IMAGE] blacklisted for session src=%s reason=%s", result.resolvedPath.c_str(),
                  interruptToReason(effectiveInterrupt, "convert-aborted"));
        }
        setRetryable(interruptToReason(effectiveInterrupt, "convert-aborted"),
                     interruptToFailureClass(effectiveInterrupt, ImageFailureClass::ConvertAborted));
        return result;
      }
      consecutiveImageFailures_++;
      setTerminal("convert-failed", ImageFailureClass::ConvertFailed);
      return result;
    }

    std::string generatedFailure;
    if (!validateCachedBmp(result.cachedBmpPath, cachedWidth, cachedHeight, generatedFailure)) {
      LOG_ERR(TAG, "[CONTENT][IMAGE] generated invalid bmp src=%s resolved=%s cached=%s reason=%s", src.c_str(),
              result.resolvedPath.c_str(), result.cachedBmpPath.c_str(), generatedFailure.c_str());
      SdMan.remove(result.cachedBmpPath.c_str());
      consecutiveImageFailures_++;
      setTerminal("generated-bmp-invalid", ImageFailureClass::GeneratedBmpInvalid);
      return result;
    }

    SdMan.remove(failedMarker.c_str());
  }  // postLock released

  consecutiveImageFailures_ = 0;
  result.width = cachedWidth;
  result.height = cachedHeight;
  result.success = true;
  result.retryable = false;
  result.status = CachedImageStatus::Success;
  result.failureClass = ImageFailureClass::None;
  result.failureReason = "generated";
  LOG_INF(TAG, "[CONTENT][IMAGE] done src=%s resolved=%s cached=%s elapsed=%lu size=%ux%u", src.c_str(),
          result.resolvedPath.c_str(), result.cachedBmpPath.c_str(),
          static_cast<unsigned long>(millis() - imageStartedMs), static_cast<unsigned>(result.width),
          static_cast<unsigned>(result.height));
  return result;
}

void ChapterHtmlSlimParser::addImageToPage(std::shared_ptr<ImageBlock> image) {
  if (stopRequested_) return;

  const int imageHeight = image->getHeight();
  const bool isTallImage = imageHeight > config.viewportHeight / 2;

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Tall images get a dedicated page: flush current page if it has content
  if (isTallImage && currentPageNextY > 0) {
    ++pagesCreated_;
    if (!completePageFn(std::move(currentPage))) {
      pendingPageImage_ = image;
      stopRequested_ = true;
      if (xmlParser_) {
        XML_StopParser(xmlParser_, XML_TRUE);  // Resumable suspend
      }
      return;
    }
    parseStartTime_ = millis();
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Check if image fits on current page
  if (currentPageNextY + imageHeight > config.viewportHeight) {
    ++pagesCreated_;
    if (!completePageFn(std::move(currentPage))) {
      pendingPageImage_ = image;
      stopRequested_ = true;
      if (xmlParser_) {
        XML_StopParser(xmlParser_, XML_TRUE);  // Resumable suspend
      }
      return;
    }
    parseStartTime_ = millis();
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Center image horizontally (cast to signed to handle images wider than viewport)
  int xPos = (static_cast<int>(config.viewportWidth) - static_cast<int>(image->getWidth())) / 2;
  if (xPos < 0) xPos = 0;

  int yPos = currentPageNextY;

  currentPage->elements.push_back(std::make_unique<PageImage>(std::move(image), xPos, yPos));
  currentPageNextY = yPos + imageHeight + effectiveLineHeight_;
}
