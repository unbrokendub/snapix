#include "CacheFs.h"

#include <FS.h>
#include <LittleFS.h>

namespace cache_fs {

bool ensureFlashDir(const std::string& path) {
  if (path.empty() || path == "/") return true;
  if (LittleFS.exists(path.c_str())) return true;

  size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos && lastSlash > 0) {
    if (!ensureFlashDir(path.substr(0, lastSlash))) return false;
  }
  return LittleFS.mkdir(path.c_str());
}

// v2.0.75: bounded recursion depth defends against pathological cache trees
// (or LittleFS bugs returning entries that point at parent dirs).  Realistic
// cache layout depth: /cache/<type>_<hash>/sections/N → 4 levels.  20 is
// generous headroom; deeper recursion silently bails to prevent stack
// overflow on the foreground task.
static constexpr int kMaxRmTreeDepth = 20;

static bool rmTreeImpl(const std::string& path, int depth) {
  if (depth > kMaxRmTreeDepth) {
    // Don't crash; refuse to recurse further and signal failure so the
    // caller knows the cleanup was incomplete.
    return false;
  }
  if (!LittleFS.exists(path.c_str())) return true;

  File dir = LittleFS.open(path.c_str(), "r");
  if (!dir) return false;
  if (!dir.isDirectory()) {
    dir.close();
    return LittleFS.remove(path.c_str());
  }

  File entry = dir.openNextFile();
  while (entry) {
    const std::string entryPath = path + "/" + entry.name();
    const bool isDir = entry.isDirectory();
    entry.close();
    if (isDir) {
      if (!rmTreeImpl(entryPath, depth + 1)) {
        dir.close();
        return false;
      }
    } else {
      LittleFS.remove(entryPath.c_str());
    }
    entry = dir.openNextFile();
  }
  dir.close();
  return LittleFS.rmdir(path.c_str());
}

bool rmTree(const std::string& path) {
  // Pattern matches the inline lambdas previously in Fb2.cpp::clearCache and
  // SettingsState.cpp's rmTree — those proved out the iterate-and-delete
  // approach against the Arduino LittleFS implementation.
  return rmTreeImpl(path, 0);
}

}  // namespace cache_fs
