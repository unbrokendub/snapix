#include "XtcProvider.h"

#include <FS.h>
#include <LittleFS.h>
#include <CacheFs.h>
#include <CoverHelpers.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Utf8.h>
#include <XtcCoverHelper.h>

#include <cstring>
#include <functional>
#include <string>

namespace snapix {

Result<void> XtcProvider::open(const char* path, const char* cacheDir) {
  close();

  xtc::XtcError err = parser.open(path);
  if (err != xtc::XtcError::OK) {
    return ErrVoid(Error::ParseFailed);
  }

  // Populate metadata
  meta.clear();
  meta.type = ContentType::Xtc;

  // v2.0.75: utf8SafeCopy for title/author (avoids splitting Cyrillic/CJK).
  std::string title = parser.getTitle();
  if (title.empty()) {
    // Use filename as title — filename is OS path so likely Latin-1, but
    // sanitize anyway in case a Cyrillic filename ever lands.
    const char* lastSlash = strrchr(path, '/');
    const char* filename = lastSlash ? lastSlash + 1 : path;
    utf8SafeCopy(meta.title, sizeof(meta.title), filename);
  } else {
    utf8SafeCopy(meta.title, sizeof(meta.title), title.c_str());
  }

  std::string author = parser.getAuthor();
  utf8SafeCopy(meta.author, sizeof(meta.author), author.empty() ? "" : author.c_str());

  // Create cache path for progress saving
  if (cacheDir && cacheDir[0] != '\0') {
    std::string pathStr(path);
    size_t hash = std::hash<std::string>{}(pathStr);
    snprintf(meta.cachePath, sizeof(meta.cachePath), "%s/xtc_%zu", cacheDir, hash);
    // v2.0.73: cache lives on LittleFS now.
    cache_fs::ensureFlashDir(meta.cachePath);
  } else {
    meta.cachePath[0] = '\0';
  }

  std::string coverPath = getCoverBmpPath();
  strncpy(meta.coverPath, coverPath.c_str(), sizeof(meta.coverPath) - 1);
  meta.coverPath[sizeof(meta.coverPath) - 1] = '\0';

  meta.totalPages = parser.getPageCount();
  meta.currentPage = 0;
  meta.progressPercent = 0;

  return Ok();
}

void XtcProvider::close() {
  parser.close();
  meta.clear();
}

uint32_t XtcProvider::pageCount() const { return parser.getPageCount(); }

uint16_t XtcProvider::tocCount() const { return parser.hasChapters() ? parser.getChapters().size() : 0; }

Result<TocEntry> XtcProvider::getTocEntry(uint16_t index) const {
  if (!parser.hasChapters() || index >= tocCount()) {
    return Err<TocEntry>(Error::InvalidState);
  }

  const auto& chapters = parser.getChapters();
  const auto& chapter = chapters[index];

  TocEntry entry;
  // v2.0.75: utf8SafeCopy for chapter titles (Cyrillic/CJK safe truncation).
  utf8SafeCopy(entry.title, sizeof(entry.title), chapter.name.c_str());
  entry.pageIndex = chapter.startPage;
  entry.depth = 0;  // XTC chapters are flat

  return Ok(entry);
}

std::string XtcProvider::getCoverBmpPath() const { return std::string(meta.cachePath) + "/cover.bmp"; }

std::string XtcProvider::getThumbBmpPath() const { return std::string(meta.cachePath) + "/thumb.bmp"; }

bool XtcProvider::generateCoverBmp() {
  const auto coverPath = getCoverBmpPath();
  // v2.0.73: cover lives on LittleFS now.
  if (LittleFS.exists(coverPath.c_str())) {
    return true;
  }
  return xtc::generateCoverBmpFromParser(parser, coverPath);
}

bool XtcProvider::generateThumbBmp() {
  const auto thumbPath = getThumbBmpPath();
  const auto failedMarkerPath = std::string(meta.cachePath) + "/.thumb.failed";

  // v2.0.73: thumb + failure marker live on LittleFS.
  if (LittleFS.exists(thumbPath.c_str())) return true;
  if (LittleFS.exists(failedMarkerPath.c_str())) return false;

  if (!LittleFS.exists(getCoverBmpPath().c_str()) && !generateCoverBmp()) {
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) marker.close();
    return false;
  }

  const bool success = CoverHelpers::generateThumbFromCover(getCoverBmpPath(), thumbPath, "XTC");
  if (!success) {
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) marker.close();
  }
  return success;
}

}  // namespace snapix
