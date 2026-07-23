/**
 * Markdown.cpp
 *
 * Markdown file handler implementation for Snapix Reader
 */

#include "Markdown.h"

#include <FS.h>
#include <LittleFS.h>
#include <CacheFs.h>
#include <CoverHelpers.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <SDCardManager.h>

// v2.0.192 — reuse the cp1251 detection + conversion code introduced
// for TXT in v2.0.187.  The namespace is `snapix::txt` (historical —
// could be renamed to `snapix::text_encoding` in a future cleanup pass)
// but the logic itself is encoding-agnostic and works for any plain-text
// format including Markdown.
#include <TxtEncoding.h>

#define TAG "MARKDOWN"

Markdown::Markdown(std::string filepath, const std::string& cacheDir)
    : filepath(std::move(filepath)), fileSize(0), loaded(false) {
  // Create cache key based on filepath (same as Epub/Xtc/Txt)
  cachePath = cacheDir + "/md_" + std::to_string(std::hash<std::string>{}(this->filepath));

  // Extract title from filename
  size_t lastSlash = this->filepath.find_last_of('/');
  size_t lastDot = this->filepath.find_last_of('.');

  if (lastSlash == std::string::npos) {
    lastSlash = 0;
  } else {
    lastSlash++;
  }

  if (lastDot == std::string::npos || lastDot <= lastSlash) {
    title = this->filepath.substr(lastSlash);
  } else {
    title = this->filepath.substr(lastSlash, lastDot - lastSlash);
  }
}

bool Markdown::load() {
  LOG_INF(TAG, "Loading Markdown: %s", filepath.c_str());

  if (!SdMan.exists(filepath.c_str())) {
    LOG_ERR(TAG, "File does not exist");
    return false;
  }

  if (!cache_fs::ensureSourceFingerprint(filepath, cachePath)) {
    LOG_ERR(TAG, "Could not verify source fingerprint");
    return false;
  }

  // Default: read directly from SD source.
  effectiveContentPath_ = filepath;
  useLittleFsForContent_ = false;

  // v2.0.192 — same cp1251 detection + UTF-8 cache routing as Txt::load
  // (introduced for .txt in v2.0.187/189).  Russian markdown files
  // exported from Windows tools (Word, older editors) often default to
  // cp1251 and render as `?` without conversion.  This handles them
  // identically to .txt — sample the first 4 KB, classify, convert to
  // a LittleFS cache file if cp1251, drop any stale pagination caches
  // on size mismatch so the next paginator pass uses the new UTF-8
  // byte stream.
  const snapix::txt::Encoding enc = snapix::txt::detectFileEncoding(filepath);
  if (enc == snapix::txt::Encoding::Cp1251) {
    setupCacheDir();
    const std::string utf8Path = cachePath + "/utf8.md";
    const std::string sizeMarkerPath = cachePath + "/utf8.size";

    // Read raw source size for the conversion-cache validity check.
    FsFile srcSizeProbe;
    size_t srcSize = 0;
    if (SdMan.openFileForRead("MD ", filepath, srcSizeProbe)) {
      srcSize = srcSizeProbe.size();
      srcSizeProbe.close();
    }

    bool cacheValid = false;
    if (LittleFS.exists(utf8Path.c_str()) && LittleFS.exists(sizeMarkerPath.c_str())) {
      File marker = LittleFS.open(sizeMarkerPath.c_str(), "r");
      if (marker) {
        size_t cachedSrcSize = 0;
        if (marker.read(reinterpret_cast<uint8_t*>(&cachedSrcSize), sizeof(cachedSrcSize)) ==
            sizeof(cachedSrcSize)) {
          cacheValid = (cachedSrcSize == srcSize);
        }
        marker.close();
      }
    }

    if (!cacheValid) {
      LOG_INF(TAG, "Generating cp1251→UTF-8 cache for %s", filepath.c_str());
      if (!snapix::txt::convertCp1251FileToUtf8(filepath, utf8Path)) {
        LOG_ERR(TAG, "cp1251 conversion failed; falling back to raw read (text may show ?)");
      } else {
        File marker = LittleFS.open(sizeMarkerPath.c_str(), "w");
        if (marker) {
          marker.write(reinterpret_cast<const uint8_t*>(&srcSize), sizeof(srcSize));
          marker.flush();
          marker.close();
        }
        // Drop any sibling `pages_*.bin` files so the reader re-paginates
        // from the new UTF-8 byte stream (offsets in the old cache were
        // computed against a different byte stream).
        File dir = LittleFS.open(cachePath.c_str(), "r");
        if (dir && dir.isDirectory()) {
          File child = dir.openNextFile();
          while (child) {
            const String name = child.name();
            const bool isPagesBin =
                name.startsWith("pages_") && name.endsWith(".bin");
            child.close();
            if (isPagesBin) {
              const std::string fullPath = cachePath + "/" + std::string(name.c_str());
              LittleFS.remove(fullPath.c_str());
              LOG_DBG(TAG, "Invalidated stale page cache: %s", fullPath.c_str());
            }
            child = dir.openNextFile();
          }
          dir.close();
        }
      }
    } else {
      LOG_DBG(TAG, "Reusing cached cp1251→UTF-8 conversion: %s", utf8Path.c_str());
    }

    if (LittleFS.exists(utf8Path.c_str())) {
      effectiveContentPath_ = utf8Path;
      useLittleFsForContent_ = true;
    }
  }

  // Report size of whatever we'll actually read (UTF-8 cache or original SD).
  if (useLittleFsForContent_) {
    File f = LittleFS.open(effectiveContentPath_.c_str(), "r");
    if (!f) {
      LOG_ERR(TAG, "Failed to open UTF-8 cache: %s", effectiveContentPath_.c_str());
      return false;
    }
    fileSize = f.size();
    f.close();
  } else {
    FsFile file;
    if (!SdMan.openFileForRead("MD ", filepath, file)) {
      LOG_ERR(TAG, "Failed to open file");
      return false;
    }
    fileSize = file.size();
    file.close();
  }

  loaded = true;

  // Try to extract title from content (updates title member if found).
  // Reads via readContent() which routes to the UTF-8 cache when set.
  extractTitleFromContent();

  LOG_INF(TAG, "Loaded Markdown: %s (%zu bytes%s)", filepath.c_str(), fileSize,
          useLittleFsForContent_ ? " — via cp1251→UTF-8 cache" : "");
  return true;
}

bool Markdown::clearCache() const {
  // v2.0.73: cache lives on LittleFS now.
  if (!LittleFS.exists(cachePath.c_str())) {
    LOG_DBG(TAG, "Cache does not exist, no action needed");
    return true;
  }
  if (!cache_fs::rmTree(cachePath)) {
    LOG_ERR(TAG, "Failed to clear cache");
    return false;
  }
  LOG_INF(TAG, "Cache cleared successfully");
  return true;
}

void Markdown::setupCacheDir() const {
  if (!cache_fs::ensureFlashDir(cachePath)) {
    LOG_ERR(TAG, "Failed to create cache dir: %s", cachePath.c_str());
  }
}

std::string Markdown::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

std::string Markdown::findCoverImage() const {
  // Extract directory path
  size_t lastSlash = filepath.find_last_of('/');
  std::string dirPath = (lastSlash == std::string::npos) ? "/" : filepath.substr(0, lastSlash);
  if (dirPath.empty()) dirPath = "/";

  return CoverHelpers::findCoverImage(dirPath, title);
}

bool Markdown::generateCoverBmp(bool use1BitDithering) const {
  const auto coverPath = getCoverBmpPath();
  const auto failedMarkerPath = cachePath + "/.cover.failed";

  // v2.0.73: cover BMP + failure marker live on LittleFS.
  if (LittleFS.exists(coverPath.c_str())) return true;
  if (LittleFS.exists(failedMarkerPath.c_str())) return false;

  std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) {
    LOG_DBG(TAG, "No cover image found");
    setupCacheDir();
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) marker.close();
    return false;
  }

  setupCacheDir();
  const bool success = CoverHelpers::convertImageToBmp(coverImagePath, coverPath, "MD ", use1BitDithering);
  if (!success) {
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) marker.close();
  }
  return success;
}

std::string Markdown::getThumbBmpPath() const { return cachePath + "/thumb.bmp"; }

bool Markdown::generateThumbBmp() const {
  const auto thumbPath = getThumbBmpPath();
  const auto failedMarkerPath = cachePath + "/.thumb.failed";

  if (LittleFS.exists(thumbPath.c_str())) return true;
  if (LittleFS.exists(failedMarkerPath.c_str())) return false;

  if (!LittleFS.exists(getCoverBmpPath().c_str()) && !generateCoverBmp(true)) {
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) marker.close();
    return false;
  }

  setupCacheDir();
  const bool success = CoverHelpers::generateThumbFromCover(getCoverBmpPath(), thumbPath, "MD ");
  if (!success) {
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) marker.close();
  }
  return success;
}

size_t Markdown::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    return 0;
  }

  // v2.0.192 — when source is cp1251, reads go through the UTF-8 cache
  // file on LittleFS instead of the SD source.  Pagination + progress
  // offsets reference the UTF-8 byte layout, kept consistent for the
  // session by the size-marker check in load().
  if (useLittleFsForContent_) {
    File f = LittleFS.open(effectiveContentPath_.c_str(), "r");
    if (!f) return 0;
    if (offset > 0) f.seek(offset);
    const size_t got = f.read(buffer, length);
    f.close();
    return got;
  }

  FsFile file;
  if (!SdMan.openFileForRead("MD ", filepath, file)) {
    return 0;
  }

  if (offset > 0) {
    file.seek(offset);
  }

  const int readResult = file.read(buffer, length);
  file.close();

  return readResult > 0 ? static_cast<size_t>(readResult) : 0;
}

bool Markdown::extractTitleFromContent() {
  // Check cache first
  std::string titleCachePath = getTitleCachePath();
  if (SdMan.exists(titleCachePath.c_str())) {
    FsFile file;
    if (SdMan.openFileForRead("MD ", titleCachePath, file)) {
      char buf[128];
      int len = file.read(buf, sizeof(buf) - 1);
      file.close();
      if (len > 0) {
        buf[len] = '\0';
        title = buf;
        return true;
      }
    }
  }

  // Read first 4KB - use heap instead of stack to avoid overflow on ESP32-C3
  constexpr size_t SCAN_SIZE = 4096;
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[SCAN_SIZE]);
  if (!buffer) return false;

  size_t bytesRead = readContent(buffer.get(), 0, SCAN_SIZE);
  if (bytesRead == 0) return false;

  // Scan for ATX header (# Title)
  std::string extracted;
  const char* p = reinterpret_cast<const char*>(buffer.get());
  const char* end = p + bytesRead;

  while (p < end) {
    // Skip to start of line
    while (p < end && (*p == '\n' || *p == '\r')) p++;
    if (p >= end) break;

    const char* lineStart = p;
    // Find end of line
    while (p < end && *p != '\n' && *p != '\r') p++;
    size_t lineLen = static_cast<size_t>(p - lineStart);

    // Check for ATX header
    if (lineLen > 1 && lineStart[0] == '#') {
      size_t hashCount = 0;
      while (hashCount < lineLen && lineStart[hashCount] == '#') hashCount++;

      if (hashCount <= 6 && hashCount < lineLen && lineStart[hashCount] == ' ') {
        // Extract title text - skip all leading whitespace after #
        size_t start = hashCount;
        while (start < lineLen && lineStart[start] == ' ') start++;
        size_t titleEnd = lineLen;
        // Strip trailing # and spaces
        while (titleEnd > start && (lineStart[titleEnd - 1] == '#' || lineStart[titleEnd - 1] == ' ')) titleEnd--;

        if (titleEnd > start) {
          extracted = std::string(lineStart + start, titleEnd - start);
          break;
        }
      }
    }
  }

  if (extracted.empty()) return false;

  // Truncate to fit buffer
  if (extracted.length() > 127) extracted.resize(127);

  // Update title
  title = extracted;

  // Cache to SD
  setupCacheDir();
  FsFile file;
  if (SdMan.openFileForWrite("MD ", titleCachePath, file)) {
    file.write(reinterpret_cast<const uint8_t*>(title.c_str()), title.length());
    file.close();
  }

  return true;
}
