/**
 * Txt.cpp
 *
 * Plain text file handler implementation for Snapix Reader
 */

#include "Txt.h"

#include <FS.h>          // Arduino base File (LittleFS, v2.0.73)
#include <LittleFS.h>    // v2.0.73: cache moved from SD to internal flash
#include <CacheFs.h>
#include <CoverHelpers.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <SDCardManager.h>

#include "TxtEncoding.h"  // v2.0.187 — cp1251 detection + conversion

#define TAG "TXT"

Txt::Txt(std::string filepath, const std::string& cacheDir)
    : filepath(std::move(filepath)), fileSize(0), loaded(false) {
  // Create cache key based on filepath (same as Epub/Xtc)
  cachePath = cacheDir + "/txt_" + std::to_string(std::hash<std::string>{}(this->filepath));

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

bool Txt::load() {
  LOG_INF(TAG, "Loading TXT: %s", filepath.c_str());

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

  // v2.0.187 — detect Windows-1251 (cp1251) source files (typical for
  // royallib.ru / lib.ru and other Russian book sites) and route reads
  // through a UTF-8 cache file on LittleFS.  Pre-fix such files
  // rendered as `?` placeholders because the downstream renderer
  // assumes UTF-8 input.
  //
  // The cache file is regenerated whenever the source file's mtime/size
  // mismatch a stored marker, so editing the .txt on the SD card and
  // re-opening picks up the changes (we use size-only here; mtime
  // tracking would need SdMan support).  Pure-ASCII / well-formed UTF-8
  // files skip conversion and read straight from SD as before.
  const snapix::txt::Encoding enc = snapix::txt::detectFileEncoding(filepath);
  if (enc == snapix::txt::Encoding::Cp1251) {
    setupCacheDir();
    const std::string utf8Path = cachePath + "/utf8.txt";
    const std::string sizeMarkerPath = cachePath + "/utf8.size";

    // Read raw source size for the conversion-cache validity check.
    FsFile srcSizeProbe;
    size_t srcSize = 0;
    if (SdMan.openFileForRead("TXT", filepath, srcSizeProbe)) {
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
        // Leave effectiveContentPath_ as the original SD source — at
        // worst the user sees the old `?` rendering rather than nothing.
      } else {
        // Persist the source-size marker so future loads skip the
        // conversion on cache hit.
        File marker = LittleFS.open(sizeMarkerPath.c_str(), "w");
        if (marker) {
          marker.write(reinterpret_cast<const uint8_t*>(&srcSize), sizeof(srcSize));
          marker.flush();
          marker.close();
        }
        // If the source size changed since previous load, the page
        // cache built off the OLD UTF-8 offsets is now stale — drop
        // any sibling `pages_*.bin` files so the reader re-paginates
        // from the new UTF-8 byte stream.  Safe to call when nothing
        // exists yet (LittleFS.remove is a no-op for missing files).
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

    // Route subsequent reads through the cached UTF-8 file (if it
    // exists; if conversion failed we fall back to the SD source).
    if (LittleFS.exists(utf8Path.c_str())) {
      effectiveContentPath_ = utf8Path;
      useLittleFsForContent_ = true;
    }
  }

  // Report size of whatever we'll actually read from (UTF-8 cache or
  // original SD file).  This is what pagination + progress restore use.
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
    if (!SdMan.openFileForRead("TXT", filepath, file)) {
      LOG_ERR(TAG, "Failed to open file");
      return false;
    }
    fileSize = file.size();
    file.close();
  }

  loaded = true;
  LOG_INF(TAG, "Loaded TXT: %s (%zu bytes%s)", filepath.c_str(), fileSize,
          useLittleFsForContent_ ? " — via cp1251→UTF-8 cache" : "");
  return true;
}

bool Txt::clearCache() const {
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

void Txt::setupCacheDir() const {
  // v2.0.73: cache lives on LittleFS now.  ensureFlashDir walks parents.
  if (!cache_fs::ensureFlashDir(cachePath)) {
    LOG_ERR(TAG, "Failed to create cache dir: %s", cachePath.c_str());
  }
}

std::string Txt::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

std::string Txt::findCoverImage() const {
  // Extract directory path
  size_t lastSlash = filepath.find_last_of('/');
  std::string dirPath = (lastSlash == std::string::npos) ? "/" : filepath.substr(0, lastSlash);
  if (dirPath.empty()) dirPath = "/";

  return CoverHelpers::findCoverImage(dirPath, title);
}

bool Txt::generateCoverBmp(bool use1BitDithering) const {
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
  const bool success = CoverHelpers::convertImageToBmp(coverImagePath, coverPath, "TXT", use1BitDithering);
  if (!success) {
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) marker.close();
  }
  return success;
}

std::string Txt::getThumbBmpPath() const { return cachePath + "/thumb.bmp"; }

bool Txt::generateThumbBmp() const {
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
  const bool success = CoverHelpers::generateThumbFromCover(getCoverBmpPath(), thumbPath, "TXT");
  if (!success) {
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) marker.close();
  }
  return success;
}

size_t Txt::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    return 0;
  }

  // v2.0.187 — when source is cp1251, reads go through the UTF-8 cache
  // file on LittleFS instead of the SD source.  The cache file's byte
  // layout is what pagination + progress restore reference, so offsets
  // are consistent across the session.
  if (useLittleFsForContent_) {
    File f = LittleFS.open(effectiveContentPath_.c_str(), "r");
    if (!f) return 0;
    if (offset > 0) f.seek(offset);
    const size_t got = f.read(buffer, length);
    f.close();
    return got;
  }

  FsFile file;
  if (!SdMan.openFileForRead("TXT", filepath, file)) {
    return 0;
  }

  if (offset > 0) {
    file.seek(offset);
  }

  const int readResult = file.read(buffer, length);
  file.close();

  return readResult > 0 ? static_cast<size_t>(readResult) : 0;
}
