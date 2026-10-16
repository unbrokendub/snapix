#include "CacheEvictionPolicy.h"

#include <algorithm>
#include <cstring>

namespace cache_fs::eviction {

namespace {

constexpr uint8_t kRecordMagic[4] = {'S', 'O', 'R', '1'};
constexpr size_t kRecordFixedBytes = 4 + 4 + 2;  // magic, seq, path length
constexpr size_t kRecordCheckBytes = 4;

constexpr const char* kBookCachePrefixes[] = {"epub", "fb2", "txt", "md", "html", "xtc"};
// std::hash<std::string> is 32 or 64 bits wide: at most 20 decimal digits.
constexpr size_t kMaxKeyDigits = 20;

uint32_t fnv1a32(const uint8_t* data, const size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

void putU16(std::vector<uint8_t>& out, const uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

void putU32(std::vector<uint8_t>& out, const uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

std::vector<uint8_t> encodeOpenRecord(const uint32_t seq, const std::string& sourcePath) {
  // A truncated path cannot be checked against the SD card: store none.
  const uint16_t storedBytes =
      sourcePath.size() <= kMaxRecordPathBytes ? static_cast<uint16_t>(sourcePath.size()) : 0;
  std::vector<uint8_t> out;
  out.reserve(kRecordFixedBytes + storedBytes + kRecordCheckBytes);
  out.insert(out.end(), kRecordMagic, kRecordMagic + sizeof(kRecordMagic));
  putU32(out, seq);
  putU16(out, storedBytes);
  out.insert(out.end(), sourcePath.begin(), sourcePath.begin() + storedBytes);
  putU32(out, fnv1a32(out.data(), out.size()));
  return out;
}

bool decodeOpenRecord(const uint8_t* data, const size_t size, uint32_t* seq, std::string* sourcePath) {
  if (!data || size < kRecordFixedBytes + kRecordCheckBytes) return false;
  if (std::memcmp(data, kRecordMagic, sizeof(kRecordMagic)) != 0) return false;
  const size_t pathBytes = static_cast<size_t>(data[8]) | (static_cast<size_t>(data[9]) << 8);
  if (pathBytes > kMaxRecordPathBytes || size != kRecordFixedBytes + pathBytes + kRecordCheckBytes) return false;
  const size_t checked = kRecordFixedBytes + pathBytes;
  if (getU32(data + checked) != fnv1a32(data, checked)) return false;
  if (seq) *seq = getU32(data + 4);
  if (sourcePath) sourcePath->assign(reinterpret_cast<const char*>(data + kRecordFixedBytes), pathBytes);
  return true;
}

bool isBookCacheDirName(const std::string& name) {
  const size_t underscore = name.find('_');
  if (underscore == std::string::npos) return false;
  const size_t digits = name.size() - underscore - 1;
  if (digits == 0 || digits > kMaxKeyDigits) return false;
  for (size_t i = underscore + 1; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
  }
  for (const char* prefix : kBookCachePrefixes) {
    if (name.compare(0, underscore, prefix) == 0 && std::strlen(prefix) == underscore) return true;
  }
  return false;
}

std::string bookCacheKey(const std::string& sourcePath) {
  // Same expression as the cachePath of Epub/Fb2/Txt/Markdown/Html/Xtc.
  return std::to_string(std::hash<std::string>{}(sourcePath));
}

bool dirNameHasKey(const std::string& name, const std::string& key) {
  if (key.empty() || !isBookCacheDirName(name) || name.size() <= key.size()) return false;
  const size_t keyStart = name.size() - key.size();
  return name[keyStart - 1] == '_' && name.compare(keyStart, key.size(), key) == 0;
}

std::vector<size_t> evictionOrder(const std::vector<Candidate>& candidates) {
  std::vector<size_t> order;
  order.reserve(candidates.size());
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (!candidates[i].isProtected && isBookCacheDirName(candidates[i].name)) order.push_back(i);
  }
  std::sort(order.begin(), order.end(), [&candidates](const size_t a, const size_t b) {
    const Candidate& lhs = candidates[a];
    const Candidate& rhs = candidates[b];
    if (lhs.orphan != rhs.orphan) return lhs.orphan;
    if (lhs.openSeq != rhs.openSeq) return lhs.openSeq < rhs.openSeq;
    return lhs.name < rhs.name;
  });
  return order;
}

PassResult runPass(const std::vector<Candidate>& candidates, Target& target, const size_t reserveBytes,
                   const size_t targetBytes, const std::function<bool()>& shouldAbort) {
  PassResult result;
  size_t freeNow = target.freeBytes();
  result.freeBefore = freeNow;
  const bool lowSpace = freeNow < reserveBytes;
  // Measuring free space walks the whole filesystem: only re-measure when a
  // decision depends on it.
  bool freeStale = false;

  for (const size_t index : evictionOrder(candidates)) {
    const Candidate& candidate = candidates[index];
    if (!candidate.orphan) {
      if (!lowSpace) break;  // orphans sort first: nothing else is due
      if (freeStale) {
        freeNow = target.freeBytes();
        freeStale = false;
      }
      if (freeNow >= targetBytes) break;
    }
    if (shouldAbort && shouldAbort()) {
      result.aborted = true;
      break;
    }
    if (target.removeBook(candidate)) {
      if (candidate.orphan) {
        ++result.orphansRemoved;
      } else {
        ++result.lruRemoved;
      }
    } else {
      ++result.failed;
    }
    freeStale = true;  // a failed removal may still have freed part of the tree
  }

  result.freeAfter = freeStale ? target.freeBytes() : freeNow;
  return result;
}

}  // namespace cache_fs::eviction
