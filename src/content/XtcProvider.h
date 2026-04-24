#pragma once

#include <Xtc/XtcParser.h>

#include <string>

#include "../core/Result.h"
#include "ContentTypes.h"

namespace snapix {

// XtcProvider wraps the XtcParser
struct XtcProvider {
  xtc::XtcParser parser;
  ContentMetadata meta;

  XtcProvider() = default;
  ~XtcProvider() = default;

  // Non-copyable
  XtcProvider(const XtcProvider&) = delete;
  XtcProvider& operator=(const XtcProvider&) = delete;

  Result<void> open(const char* path, const char* cacheDir);
  void close();

  uint32_t pageCount() const;
  uint16_t tocCount() const;
  Result<TocEntry> getTocEntry(uint16_t index) const;

  // Cover and thumbnail
  std::string getCoverBmpPath() const;
  std::string getThumbBmpPath() const;
  bool generateCoverBmp();
  bool generateThumbBmp();

  // Direct page access
  xtc::XtcParser& getParser() { return parser; }
  const xtc::XtcParser& getParser() const { return parser; }
};

}  // namespace snapix
