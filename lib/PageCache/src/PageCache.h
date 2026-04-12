#pragma once

#include <RenderConfig.h>
#include <SdFat.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ContentParser.h"  // For AbortCallback

class ContentParser;
class GfxRenderer;
class Page;

/**
 * Unified page cache for all content types (EPUB, TXT, Markdown).
 * Supports partial caching - only caches N pages at a time.
 */
class PageCache {
 public:
  struct ProbeResult {
    bool exists = false;
    bool valid = false;
    bool partial = false;
    uint16_t pageCount = 0;
  };

  // Default number of pages to cache initially
  static constexpr uint16_t DEFAULT_CACHE_CHUNK = 5;
  // Extend cache when within this many pages of the end
  static constexpr uint16_t EXTEND_THRESHOLD = 3;
  // Resident in-memory cache of deserialized pages for snappier turns
  static constexpr uint8_t RESIDENT_PAGE_LIMIT = 4;

 private:
  struct ResidentPage {
    uint16_t pageNum = 0;
    uint32_t useToken = 0;
    std::shared_ptr<Page> page;
  };

  std::string cachePath_;
  FsFile file_;
  FsFile readFile_;
  uint16_t pageCount_ = 0;
  bool isPartial_ = false;
  RenderConfig config_;
  uint32_t lutOffset_ = 0;  // Cached LUT offset for extend operations
  uint32_t readFileSize_ = 0;
  std::vector<uint32_t> pageLut_;
  std::vector<ResidentPage> residentPages_;
  uint32_t residentUseClock_ = 0;

  bool writeHeader(bool isPartial);
  bool writeLut(const std::vector<uint32_t>& lut);
  bool loadLut(std::vector<uint32_t>& lut);  // Load existing LUT for extend
  bool ensureReadHandle();
  void closeReadHandle();
  std::shared_ptr<Page> getResidentPage(uint16_t pageNum);
  void putResidentPage(uint16_t pageNum, std::shared_ptr<Page> page);
  bool ensureLutLoaded();

 public:
  explicit PageCache(std::string cachePath);
  ~PageCache() = default;

  /**
   * Try to load existing cache from disk.
   * @param config Render config to validate against
   * @return true if valid cache exists and was loaded
   */
  bool load(const RenderConfig& config);

  /**
   * Inspect cache header without mutating PageCache instance state.
   * Used by reader-side schedulers to decide whether future sections need work
   * without spamming logs or repeatedly allocating/clearing cache objects.
   */
  static ProbeResult probe(const std::string& cachePath, const RenderConfig& config, bool cleanupInvalid = true);

  /**
   * Load cache header without config validation (for dump/debug tools).
   * @return true if valid cache exists and header was read
   */
  bool loadRaw();

  /**
   * Create cache by parsing content.
   * @param parser Content parser to use
   * @param config Render config
   * @param maxPages Maximum pages to cache (0 = unlimited)
   * @param skipPages Skip serializing first N pages (for extend)
   * @param shouldAbort Optional callback to check for cancellation
   * @return true on success
   */
  bool create(ContentParser& parser, const RenderConfig& config, uint16_t maxPages = DEFAULT_CACHE_CHUNK,
              uint16_t skipPages = 0, const AbortCallback& shouldAbort = nullptr);

  /**
   * Extend cache with more pages.
   * Re-parses content but skips already-cached pages, then appends new pages.
   * @param parser Content parser (will be reset)
   * @param additionalPages Number of additional pages to cache
   * @param shouldAbort Optional callback to check for cancellation
   * @return true on success
   */
  bool extend(ContentParser& parser, uint16_t additionalPages = DEFAULT_CACHE_CHUNK,
              const AbortCallback& shouldAbort = nullptr);

  /**
   * Load a specific page from cache.
   * @param pageNum Page number (0-indexed)
   * @return Page object or nullptr on error
   */
  std::shared_ptr<Page> loadPage(uint16_t pageNum);
  void prefetchWindow(uint16_t centerPage, int direction, uint8_t span = 1);
  void trimResidentPages(uint16_t centerPage, uint8_t keepBehind, uint8_t keepAhead);
  void clearResidentPages();
  size_t residentPageCount() const { return residentPages_.size(); }

  /**
   * Clear cache from disk.
   * @return true on success
   */
  bool clear();

  // Accessors
  uint16_t pageCount() const { return pageCount_; }
  bool isPartial() const { return isPartial_; }
  bool needsExtension(uint16_t currentPage) const {
    return isPartial_ && pageCount_ > 0 && currentPage + EXTEND_THRESHOLD >= pageCount_;
  }
  const std::string& path() const { return cachePath_; }
};
