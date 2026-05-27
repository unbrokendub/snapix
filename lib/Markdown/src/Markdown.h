/**
 * Markdown.h
 *
 * Markdown file handler for Snapix Reader
 * Provides EPUB-like interface for Markdown file handling
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

/**
 * Markdown File Handler
 *
 * Handles Markdown file loading, content streaming, and cover image discovery.
 * Interface designed to be similar to Epub/Xtc/Txt classes for easy integration.
 */
class Markdown {
  std::string filepath;
  std::string cachePath;
  std::string title;
  size_t fileSize;
  bool loaded;

  // v2.0.192 — when the source file is Windows-1251 (e.g., Russian .md
  // exported from a cp1251 source), load() generates a UTF-8 cache file
  // on LittleFS and routes readContent() through it.  Mirrors the Txt
  // pattern introduced in v2.0.187/189.
  //   * For UTF-8/ASCII source: effective path == filepath (SD card)
  //   * For cp1251 source: effective path == cachePath + "/utf8.md" (LittleFS)
  std::string effectiveContentPath_;
  bool useLittleFsForContent_ = false;

 public:
  explicit Markdown(std::string filepath, const std::string& cacheDir);
  ~Markdown() = default;

  /**
   * Load Markdown file (verify existence and get size)
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

  // v2.0.192 — content-read routing for cp1251 sources (see Txt.h for
  // the same pattern).  For UTF-8/ASCII these return (filepath, false) —
  // behaviour unchanged.  For cp1251 sources they return the LittleFS
  // UTF-8 cache path + true.  Safe to call before load() — falls back
  // to the original filepath / false until load() succeeds.
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
   * Find a cover image in the same directory as the Markdown file
   * Searches for: <filename>.jpg, <filename>.bmp, cover.jpg, cover.bmp
   * @return Path to cover image, or empty string if not found
   */
  std::string findCoverImage() const;

  // Check if file is loaded
  bool isLoaded() const { return loaded; }

 private:
  bool extractTitleFromContent();
  std::string getTitleCachePath() const { return cachePath + "/title.txt"; }
};
