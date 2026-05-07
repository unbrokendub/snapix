/**
 * Fb2.h
 *
 * FictionBook 2.0 XML e-book handler for Snapix Reader
 * Provides EPUB-like interface for FB2 file handling
 */

#pragma once

#include <expat.h>

#include <climits>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * Fb2 File Handler
 *
 * Handles FB2 file loading, XML parsing, and metadata extraction.
 */
class Fb2 {
 public:
  struct TocItem {
    std::string title;
    int sectionIndex = -1;  // Sequential section number (0-based)
    uint32_t sourceOffset = 0;  // Byte offset of the section start in the FB2 source
    uint8_t depth = 0;  // Nesting level in the FB2 section tree
  };

  struct BinaryEntry {
    uint32_t fileOffset = 0;  // Byte offset where base64 content starts in FB2 source
                              // (i.e. immediately after the opening <binary ...> tag).
    uint32_t byteLength = 0;  // Length of the base64 content in source bytes
                              // (between the opening tag and </binary>).
    uint8_t mimeType = 0;     // 0 = image/jpeg, 1 = image/png, 2 = other
  };

 private:
  std::string filepath;
  std::string cachePath;
  std::string title;
  std::string author;
  std::string coverPath;
  size_t fileSize;
  bool loaded;

  // XML parsing state
  XML_Parser xmlParser_ = nullptr;
  int depth = 0;
  int skipUntilDepth = INT_MAX;  // Skip content inside binary tags

  // Metadata extraction state
  bool inBookTitle = false;
  bool inFirstName = false;
  bool inLastName = false;
  bool inAuthor = false;
  bool inTitleInfo = false;
  bool inCoverPage = false;
  std::string currentAuthorFirst;
  std::string currentAuthorLast;

  // Body tracking (for TOC section counting)
  bool inBody = false;
  int bodyCount_ = 0;

  // TOC extraction state (tocItems_ used only during initial parse, then freed)
  std::vector<TocItem> tocItems_;
  // Lazy TOC: compact LUT of file offsets into meta.bin (4 bytes/entry vs ~68 bytes/entry)
  std::vector<uint32_t> tocLut_;
  uint16_t tocItemCount_ = 0;
  int sectionCounter_ = 0;
  int sectionDepth_ = 0;
  uint32_t currentSectionOffset_ = 0;
  bool inSectionTitle_ = false;
  int sectionTitleDepth_ = 0;
  std::string currentSectionTitle_;

  // Binary (image) index: binary id → file offset / length of base64 data.
  // Built during the initial XML parse and persisted in meta.bin.  Lets the
  // chapter parser jump directly to a referenced <image l:href="#id"/> at
  // render time without re-scanning the whole file.
  std::unordered_map<std::string, BinaryEntry> binaryIndex_;
  // Tracking state during parse — which <binary> block we're currently inside
  // (empty when not in a binary block).
  std::string currentBinaryId_;
  uint8_t currentBinaryMime_ = 0;
  uint32_t currentBinaryStart_ = 0;

  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  // Helper methods
  bool parseXmlStream();
  void postProcessMetadata();
  bool loadMetaCache();
  bool saveMetaCache() const;
  std::string metaCachePath() const;

 public:
  explicit Fb2(std::string filepath, const std::string& cacheDir);
  ~Fb2();

  /**
   * Load FB2 file (verify existence and parse metadata)
   * @return true on success
   */
  bool load();

  /**
   * Clear cached data
   * @return true on success
   */
  bool clearCache() const;

  /**
   * Setup cache directory
   */
  void setupCacheDir() const;

  // Path accessors
  const std::string& getCachePath() const { return cachePath; }
  const std::string& getPath() const { return filepath; }

  // Metadata
  const std::string& getTitle() const { return title; }
  const std::string& getAuthor() const { return author; }
  size_t getFileSize() const { return fileSize; }

  // Cover image support
  std::string getCoverBmpPath() const;
  bool generateCoverBmp(bool use1BitDithering = false) const;
  std::string getThumbBmpPath() const;
  bool generateThumbBmp() const;

  /**
   * Read content from file at specified offset
   * @param buffer Output buffer
   * @param offset Byte offset in file
   * @param length Number of bytes to read
   * @return Number of bytes actually read
   */
  size_t readContent(uint8_t* buffer, size_t offset, size_t length) const;

  /**
   * Find a cover image in the same directory as the FB2 file
   * @return Path to cover image, or empty string if not found
   */
  std::string findCoverImage() const;

  // Check if file is loaded
  bool isLoaded() const { return loaded; }

  // TOC access (lazy: reads from cache file on demand)
  uint16_t tocCount() const { return tocItemCount_; }
  TocItem getTocItem(uint16_t index) const;

  // Binary (image) index access — returns nullptr if id not found.
  const BinaryEntry* findBinary(const std::string& id) const {
    auto it = binaryIndex_.find(id);
    return (it != binaryIndex_.end()) ? &it->second : nullptr;
  }
  size_t binaryCount() const { return binaryIndex_.size(); }

  /**
   * Decode a referenced binary block to a cached BMP on SD card.  Idempotent:
   * if the cached BMP already exists with sane dimensions, returns its path
   * without re-decoding.  Used by the chapter parser to materialise inline
   * <image> references during page layout.
   *
   * @param binaryId       The <binary id> referenced by an <image l:href="#id"/>
   * @param outBmpPath     On success: filesystem path to the cached BMP
   * @param outWidth       On success: BMP width in pixels
   * @param outHeight      On success: BMP height in pixels
   * @param maxBoxWidth    Output BMP scaled to fit at most this many pixels wide
   * @param maxBoxHeight   ...and at most this many pixels tall
   * @return true on success, false if the binary is missing / cannot be decoded
   */
  bool cacheImage(const std::string& binaryId, std::string& outBmpPath, uint16_t& outWidth, uint16_t& outHeight,
                  int maxBoxWidth, int maxBoxHeight) const;
};
