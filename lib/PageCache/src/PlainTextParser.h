#pragma once

#include <RenderConfig.h>
#include <ScriptDetector.h>
#include <SdFat.h>

#include <string>

#include "ContentParser.h"

class GfxRenderer;
class Page;
class ParsedText;

/**
 * Content parser for plain text files (TXT, Markdown).
 * Reads text, wraps into lines, and creates Page objects.
 */
class PlainTextParser : public ContentParser {
  std::string filepath_;
  GfxRenderer& renderer_;
  RenderConfig config_;
  size_t fileSize_ = 0;
  size_t currentOffset_ = 0;
  bool hasMore_ = true;
  bool isRtl_ = false;
  // v2.0.189 — when true, `filepath_` lives on LittleFS (typically the
  // UTF-8 cache file generated from a cp1251 source by Txt::load()).
  // Open via LittleFS.open() instead of SdMan.openFileForRead().
  bool useLittleFs_ = false;

  // Carries over unconsumed words from a paragraph that was
  // interrupted by a page-batch limit.
  std::unique_ptr<ParsedText> pendingBlock_;
  std::unique_ptr<Page> pendingPage_;
  int16_t pendingPageY_ = 0;

 public:
  // v2.0.189 — `useLittleFs` defaults to false for source compatibility
  // with existing callers (Markdown, tools, tests).  TXT callers that
  // routed through a cp1251→UTF-8 cache must pass true (and the cache
  // path) so reads come from LittleFS, not SD.
  PlainTextParser(std::string filepath, GfxRenderer& renderer, const RenderConfig& config,
                  bool useLittleFs = false);
  ~PlainTextParser() override = default;

  bool parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages = 0,
                  const AbortCallback& shouldAbort = nullptr) override;
  bool hasMoreContent() const override { return hasMore_; }
  bool canResume() const override { return currentOffset_ > 0 && hasMore_; }
  void reset() override;
};
