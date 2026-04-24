#pragma once

#include <Html.h>

#include <memory>

#include "../core/Result.h"
#include "ContentTypes.h"

namespace snapix {

struct HtmlProvider {
  std::unique_ptr<Html> html;
  ContentMetadata meta;

  HtmlProvider() = default;
  ~HtmlProvider() = default;

  HtmlProvider(const HtmlProvider&) = delete;
  HtmlProvider& operator=(const HtmlProvider&) = delete;

  HtmlProvider(HtmlProvider&&) = default;
  HtmlProvider& operator=(HtmlProvider&&) = default;

  Result<void> open(const char* path, const char* cacheDir);
  void close();

  uint32_t pageCount() const;
  uint16_t tocCount() const { return 0; }
  Result<TocEntry> getTocEntry(uint16_t index) const { return Err<TocEntry>(Error::InvalidState); }

  Html* getHtml() { return html.get(); }
  const Html* getHtml() const { return html.get(); }
};

}  // namespace snapix
