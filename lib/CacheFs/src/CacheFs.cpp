#include "CacheFs.h"

#include <FS.h>
#include <LittleFS.h>
#include <SDCardManager.h>
#include <SharedSpiLock.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace cache_fs {

namespace {

constexpr uint32_t kFingerprintMagic = 0x31504653;  // "SFP1"
constexpr uint16_t kFingerprintVersion = 1;
constexpr size_t kSampleBytes = 4096;

struct SourceFingerprint {
  uint32_t magic = kFingerprintMagic;
  uint16_t version = kFingerprintVersion;
  uint16_t reserved = 0;
  uint64_t pathHash = 0;
  uint64_t fileSize = 0;
  uint32_t modifiedDateTime = 0;
  uint32_t reserved2 = 0;
  uint64_t sampleHash = 0;
};

static_assert(sizeof(SourceFingerprint) == 40, "SourceFingerprint layout changed");

uint64_t fnv1aUpdate(uint64_t hash, const uint8_t* data, size_t size) {
  constexpr uint64_t kPrime = 1099511628211ULL;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= kPrime;
  }
  return hash;
}

uint64_t fnv1a(const std::string& value) {
  constexpr uint64_t kOffset = 14695981039346656037ULL;
  return fnv1aUpdate(kOffset, reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

bool readSourceFingerprint(const std::string& sourcePath, SourceFingerprint& out) {
  FsFile file;
  if (!SdMan.openFileForRead("CACHE_FP", sourcePath, file)) return false;
  snapix::spi::SharedBusLock busLock;

  out = SourceFingerprint{};
  out.pathHash = fnv1a(sourcePath);
  out.fileSize = file.size();

  uint16_t date = 0;
  uint16_t time = 0;
  if (file.getModifyDateTime(&date, &time)) {
    out.modifiedDateTime = (static_cast<uint32_t>(date) << 16) | time;
  }

  std::array<uint8_t, kSampleBytes> buffer{};
  constexpr uint64_t kOffset = 14695981039346656037ULL;
  uint64_t sampleHash = kOffset;
  const uint64_t fileSize = out.fileSize;
  const std::array<uint64_t, 3> offsets = {
      0,
      fileSize > kSampleBytes ? (fileSize - std::min<uint64_t>(fileSize, kSampleBytes)) / 2 : 0,
      fileSize > kSampleBytes ? fileSize - kSampleBytes : 0,
  };

  uint64_t previousOffset = UINT64_MAX;
  for (const uint64_t offset : offsets) {
    if (offset == previousOffset) continue;
    previousOffset = offset;
    if (!file.seekSet(offset)) {
      file.close();
      return false;
    }
    const size_t wanted =
        static_cast<size_t>(std::min<uint64_t>(kSampleBytes, fileSize - offset));
    const int got = file.read(buffer.data(), wanted);
    if (got < 0 || static_cast<size_t>(got) != wanted) {
      file.close();
      return false;
    }
    sampleHash = fnv1aUpdate(sampleHash, reinterpret_cast<const uint8_t*>(&offset),
                             sizeof(offset));
    sampleHash = fnv1aUpdate(sampleHash, buffer.data(), wanted);
  }
  file.close();
  out.sampleHash = sampleHash;
  return true;
}

bool fingerprintEquals(const SourceFingerprint& lhs, const SourceFingerprint& rhs) {
  return lhs.magic == kFingerprintMagic && lhs.version == kFingerprintVersion &&
         lhs.pathHash == rhs.pathHash && lhs.fileSize == rhs.fileSize &&
         lhs.modifiedDateTime == rhs.modifiedDateTime && lhs.sampleHash == rhs.sampleHash;
}

}  // namespace

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

bool ensureSourceFingerprint(const std::string& sourcePath, const std::string& cachePath) {
  if (sourcePath.empty() || cachePath.empty()) return false;

  SourceFingerprint current{};
  if (!readSourceFingerprint(sourcePath, current)) return false;

  const std::string fingerprintPath = cachePath + "/source.fp";
  SourceFingerprint cached{};
  bool cacheMatches = false;
  if (LittleFS.exists(fingerprintPath.c_str())) {
    File marker = LittleFS.open(fingerprintPath.c_str(), "r");
    if (marker) {
      const size_t got = marker.read(reinterpret_cast<uint8_t*>(&cached), sizeof(cached));
      marker.close();
      cacheMatches = got == sizeof(cached) && fingerprintEquals(cached, current);
    }
  }
  if (cacheMatches) return true;

  // An unversioned cache may have been produced by older firmware and cannot
  // be proven to match the SD source.  Rebuild it once instead of risking
  // silently serving stale page/metadata data.
  if (!rmTree(cachePath) || !ensureFlashDir(cachePath)) return false;

  const std::string tmpPath = fingerprintPath + ".tmp";
  LittleFS.remove(tmpPath.c_str());
  File marker = LittleFS.open(tmpPath.c_str(), "w");
  if (!marker) return false;
  const size_t written = marker.write(reinterpret_cast<const uint8_t*>(&current), sizeof(current));
  marker.flush();
  marker.close();
  if (written != sizeof(current)) {
    LittleFS.remove(tmpPath.c_str());
    return false;
  }
  LittleFS.remove(fingerprintPath.c_str());
  if (!LittleFS.rename(tmpPath.c_str(), fingerprintPath.c_str())) {
    LittleFS.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

}  // namespace cache_fs
