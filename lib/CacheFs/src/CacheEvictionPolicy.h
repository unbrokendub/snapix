#pragma once

// v3.11.12 — least-recently-opened eviction of per-book caches on the internal
// LittleFS partition: the policy half.  No LittleFS, SD or FreeRTOS here, so
// host tests drive it; CacheEviction.cpp binds it to the device filesystem.
//
// Each opened book leaves a cache tree on the 9.6 MB partition (an image-heavy
// EPUB takes several MB) and nothing else ever deletes one, so a full partition
// made a new book hang on "Loading..." or open without images.  Evicting a
// cache only costs a rebuild: progress, bookmarks and settings live on SD.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cache_fs::eviction {

// A pass starts when free space drops below the reserve and evicts until the
// target is free.  The gap is hysteresis: one pass makes room for a few book
// opens instead of running on every one.
constexpr size_t kReserveFreeBytes = 2u * 1024u * 1024u;
constexpr size_t kTargetFreeBytes = 3u * 1024u * 1024u;

// The "last opened" record, one small file in each book's cache dir.  The
// device has no reliable clock, so recency is an open sequence number drawn
// from a counter file in the cache root.  The record also names the source
// file, so a cache whose book is gone from the SD card can be recognised.
constexpr char kOpenRecordFile[] = "open.rec";
constexpr char kOpenCounterFile[] = "open.seq";
constexpr size_t kMaxRecordPathBytes = 512;

std::vector<uint8_t> encodeOpenRecord(uint32_t seq, const std::string& sourcePath);
bool decodeOpenRecord(const uint8_t* data, size_t size, uint32_t* seq, std::string* sourcePath);

// "<format>_<decimal hash>": the names Epub, Fb2, Txt, Markdown, Html and Xtc
// give their cache dirs (also mirrored under /img by Fb2).  Nothing else in the
// cache root is ever evicted.
bool isBookCacheDirName(const std::string& name);
// The decimal hash part.  Every format derives it from the source path the
// same way, so it identifies one book's dirs whatever its format.
std::string bookCacheKey(const std::string& sourcePath);
bool dirNameHasKey(const std::string& name, const std::string& key);

struct Candidate {
  std::string name;          // dir name under the cache root (or /img only)
  uint32_t openSeq = 0;      // 0: no record — older firmware or a half-removed dir
  bool orphan = false;       // the source is gone, or only a dead /img dir is left
  bool isProtected = false;  // the book being opened or read: never evicted
};

// Orphans first, then least recently opened first; ties by name so the order is
// deterministic.  Protected entries and non-book names are left out.
std::vector<size_t> evictionOrder(const std::vector<Candidate>& candidates);

// The filesystem side of a pass.
class Target {
 public:
  virtual ~Target() = default;
  virtual size_t freeBytes() = 0;
  // Removes the whole cache of one book; false if anything was left behind.
  virtual bool removeBook(const Candidate& candidate) = 0;
};

struct PassResult {
  size_t freeBefore = 0;
  size_t freeAfter = 0;
  uint16_t orphansRemoved = 0;
  uint16_t lruRemoved = 0;
  uint16_t failed = 0;
  bool aborted = false;
};

// Orphans always go.  Other caches go, least recently opened first, only when
// the pass started below `reserveBytes`, and only until `targetBytes` is free.
// `shouldAbort` is checked before each removal.
PassResult runPass(const std::vector<Candidate>& candidates, Target& target, size_t reserveBytes,
                   size_t targetBytes, const std::function<bool()>& shouldAbort = {});

}  // namespace cache_fs::eviction
