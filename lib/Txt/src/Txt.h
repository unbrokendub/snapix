/**
 * Txt.h
 *
 * Plain text file handler for Snapix Reader
 * Provides EPUB-like interface for TXT file handling
 */

#pragma once

#include <string>

/**
 * TXT File Handler
 *
 * Handles TXT file loading, content streaming, and cover image discovery.
 * Interface designed to be similar to Epub/Xtc classes for easy integration.
 */
class Txt {
  std::string filepath;
  std::string cachePath;
  std::string title;
  size_t fileSize;
  bool loaded;

  // v2.0.187 — when the source file is Windows-1251, load() generates
  // a UTF-8 cache file on LittleFS and routes readContent() through it.
  // `effectiveContentPath_` is the path actually used for reads:
  //   * For UTF-8/ASCII source: same as `filepath` (SD card).
  //   * For cp1251 source: cachePath + "/utf8.txt" (LittleFS).
  // `useLittleFsForContent_` selects the right FS adapter in readContent().
  std::string effectiveContentPath_;
  bool useLittleFsForContent_ = false;

 public:
  explicit Txt(std::string filepath, const std::string& cacheDir);
  ~Txt() = default;

  /**
   * Load TXT file (verify existence and get size)
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

  // v2.0.189 — content-read routing for cp1251 sources.  load() may
  // route reads through a UTF-8 cache file on LittleFS when the SD
  // source is Windows-1251.  Downstream paginators (PlainTextParser)
  // need both the effective path AND the FS choice so they read the
  // SAME byte stream that load() will report via getFileSize().
  //
  // For pure UTF-8/ASCII sources these return `filepath` + `false`
  // (i.e. read from SD as before), so callers can use the same code
  // path regardless of encoding.  Safe to call before load() — falls
  // back to the original SD filepath / `false` until load() succeeds.
  const std::string& getEffectiveContentPath() const {
    return effectiveContentPath_.empty() ? filepath : effectiveContentPath_;
  }
  bool isContentOnLittleFs() const { return useLittleFsForContent_; }

  // Metadata
  const std::string& getTitle() const { return title; }
  size_t getFileSize() const { return fileSize; }

  // Cover image support (for sleep screen and home view)
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
   * Find a cover image in the same directory as the TXT file
   * Searches for: <filename>.jpg, <filename>.bmp, cover.jpg, cover.bmp
   * @return Path to cover image, or empty string if not found
   */
  std::string findCoverImage() const;

  // Check if file is loaded
  bool isLoaded() const { return loaded; }
};
