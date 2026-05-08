#include "BookmarkManager.h"

#include <Arduino.h>
#include <Logging.h>
#include <SdFat.h>

#include <cstdio>
#include <cstring>

#include "../config.h"
#include "../core/Core.h"

#define TAG "BOOKMARK"

namespace snapix {

namespace {
// v2.0.61: bookmarks live on SD under /.snapix/bookmarks/<book_id>.bin
// (was /<cacheDir>/bookmarks.bin, but cacheDir moved to LittleFS in v2.0.60
// while user data stays on SD).  Same book_id extraction as ProgressManager.
void buildBookmarkPath(char* out, size_t outSize, const char* cacheDir, const char* ext) {
  const char* slash = std::strrchr(cacheDir, '/');
  const char* bookId = slash ? slash + 1 : cacheDir;
  snprintf(out, outSize, "%s/bookmarks/%s.%s", SNAPIX_DIR, bookId, ext);
}
}  // namespace

bool BookmarkManager::save(Core& core, const char* cacheDir, ContentType type, const Bookmark* bookmarks, int count) {
  if (!cacheDir || cacheDir[0] == '\0') return false;
  if (count < 0 || count > MAX_BOOKMARKS) return false;

  // Make sure the SD bookmarks directory exists.
  char bmDir[280];
  snprintf(bmDir, sizeof(bmDir), "%s/bookmarks", SNAPIX_DIR);
  core.storage.mkdir(bmDir);

  char path[280];
  buildBookmarkPath(path, sizeof(path), cacheDir, "bin");

  FsFile file;
  auto result = core.storage.openWrite(path, file);
  if (!result.ok()) {
    LOG_ERR(TAG, "Failed to save bookmarks to %s", path);
    return false;
  }

  uint8_t cnt = static_cast<uint8_t>(count);
  file.write(&cnt, 1);
  if (count > 0) {
    file.write(reinterpret_cast<const uint8_t*>(bookmarks), static_cast<size_t>(count) * sizeof(Bookmark));
  }
  file.close();
  LOG_DBG(TAG, "Saved %d bookmarks", count);

  buildBookmarkPath(path, sizeof(path), cacheDir, "txt");
  result = core.storage.openWrite(path, file);
  if (!result.ok()) {
    LOG_ERR(TAG, "Failed to export bookmarks.txt");
    return true;
  }

  char line[128];
  for (int i = 0; i < count; i++) {
    const Bookmark& b = bookmarks[i];
    int len = 0;
    if (type == ContentType::Epub || type == ContentType::Fb2) {
      len = snprintf(line, sizeof(line), "Ch %d, Page %d: %s\n", b.spineIndex + 1, b.sectionPage + 1, b.label);
    } else if (type == ContentType::Xtc) {
      len = snprintf(line, sizeof(line), "Page %u: %s\n", static_cast<unsigned>(b.flatPage + 1), b.label);
    } else {
      len = snprintf(line, sizeof(line), "Page %d: %s\n", b.sectionPage + 1, b.label);
    }
    if (len > 0) {
      file.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(len));
    }
  }
  file.close();

  return true;
}

int BookmarkManager::load(Core& core, const char* cacheDir, Bookmark* bookmarks, int maxCount) {
  if (!cacheDir || cacheDir[0] == '\0') return 0;

  char path[280];
  buildBookmarkPath(path, sizeof(path), cacheDir, "bin");

  FsFile file;
  auto result = core.storage.openRead(path, file);
  if (!result.ok()) {
    LOG_DBG(TAG, "No saved bookmarks found");
    return 0;
  }

  if (file.size() < 1) {
    LOG_ERR(TAG, "Corrupted bookmarks file (too small)");
    file.close();
    return 0;
  }

  uint8_t cnt;
  if (file.read(&cnt, 1) != 1) {
    LOG_ERR(TAG, "Failed to read bookmark count");
    file.close();
    return 0;
  }

  int toLoad = cnt;
  if (toLoad > maxCount) toLoad = maxCount;
  if (toLoad > MAX_BOOKMARKS) toLoad = MAX_BOOKMARKS;

  if (toLoad > 0) {
    size_t expected = static_cast<size_t>(toLoad) * sizeof(Bookmark);
    if (file.size() - 1 < static_cast<int64_t>(expected)) {
      LOG_ERR(TAG, "Corrupted bookmarks file (truncated)");
      file.close();
      return 0;
    }
    int bytesRead = file.read(reinterpret_cast<uint8_t*>(bookmarks), expected);
    if (bytesRead != static_cast<int>(expected)) {
      LOG_ERR(TAG, "Failed to read bookmarks data");
      file.close();
      return 0;
    }
  }

  file.close();
  LOG_DBG(TAG, "Loaded %d bookmarks", toLoad);
  return toLoad;
}

int BookmarkManager::findAt(const Bookmark* bookmarks, int count, ContentType type, int spineIndex, int sectionPage,
                            uint32_t flatPage) {
  for (int i = 0; i < count; i++) {
    if (type == ContentType::Epub || type == ContentType::Fb2) {
      if (bookmarks[i].spineIndex == spineIndex && bookmarks[i].sectionPage == sectionPage) {
        return i;
      }
    } else if (type == ContentType::Xtc) {
      if (bookmarks[i].flatPage == flatPage) {
        return i;
      }
    } else {
      if (bookmarks[i].sectionPage == sectionPage) {
        return i;
      }
    }
  }
  return -1;
}

}  // namespace snapix
