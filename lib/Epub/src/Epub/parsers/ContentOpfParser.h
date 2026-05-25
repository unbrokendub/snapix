#pragma once
#include <FS.h>          // Arduino base File (LittleFS, v2.0.73)
#include <Print.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "Epub.h"
#include "expat.h"

constexpr size_t MAX_TITLE_LENGTH = 256;
constexpr size_t MAX_AUTHOR_LENGTH = 128;

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  // v2.0.73: temp item store moved from SD to LittleFS along with the rest
  // of the EPUB cache.
  File tempItemStore;
  // v2.0.179 — converted from `std::unordered_map<string,string>` to a
  // sorted vector to drop the ~40 B/entry hash-bucket overhead.  Built
  // once during the OPF manifest pass, then queried during the spine
  // pass; entries are immutable after parse so a sorted vector with
  // binary search matches the usage pattern exactly.  For typical
  // EPUBs (~30-150 manifest items) the vector representation saves
  // ~1-6 KB transient heap during book open.
  using ManifestEntry = std::pair<std::string, std::string>;  // itemId -> href
  std::vector<ManifestEntry> manifestIndex;
  bool manifestIndexSorted = false;

  // Insert during the manifest pass (unsorted append for O(1) build).
  void manifestInsert(std::string itemId, std::string href) {
    manifestIndex.emplace_back(std::move(itemId), std::move(href));
    manifestIndexSorted = false;
  }

  // Look up during the spine pass (lazily sorts on first lookup).
  const std::string* manifestFind(const std::string& itemId) {
    if (!manifestIndexSorted) {
      std::sort(manifestIndex.begin(), manifestIndex.end(),
                [](const ManifestEntry& a, const ManifestEntry& b) { return a.first < b.first; });
      manifestIndexSorted = true;
    }
    auto it = std::lower_bound(
        manifestIndex.begin(), manifestIndex.end(), itemId,
        [](const ManifestEntry& entry, const std::string& key) { return entry.first < key; });
    if (it != manifestIndex.end() && it->first == itemId) return &it->second;
    return nullptr;
  }
  std::string coverItemId;
  std::vector<std::string> cssFiles_;

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  std::string title;
  std::string author;
  std::string language;
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string textReferenceHref;
  const std::vector<std::string>& getCssFiles() const { return cssFiles_; }

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache)
      : cachePath(cachePath), baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
