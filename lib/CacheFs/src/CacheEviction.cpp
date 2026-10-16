#include "CacheEviction.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <Logging.h>
#include <SDCardManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <climits>
#include <cstring>
#include <mutex>
#include <vector>

#include "CacheEvictionPolicy.h"
#include "CacheSpaceHook.h"

#define TAG "CACHE_EVICT"

namespace cache_fs::eviction {

namespace {

// Fb2 keeps its BMPs in "/img/<same dir name>" (kFlashImageCacheRoot).
constexpr char kImageRoot[] = "/img";
// Written by ensureSourceFingerprint; removed first when evicting, so a removal
// cut short (abort, power loss) leaves a cache the book's next open rebuilds.
constexpr char kSourceFingerprintFile[] = "source.fp";
// A pass started by a failed write runs on the writer's task (a page turn may
// be waiting for it): it stops starting removals after this long and leaves
// the rest to the background worker.
constexpr uint32_t kWriteFailurePassBudgetMs = 1500;
// Measuring free space walks the whole filesystem under its lock.  A failed
// write shortly after a measurement that showed enough room is no space issue.
constexpr uint32_t kFreeCheckReuseMs = 2000;
// "/cache/<book>/sections/<file>" is three levels below the root.
constexpr int kMaxTreeDepth = 8;
constexpr size_t kMaxRecordBytes = 4 + 4 + 2 + kMaxRecordPathBytes + 4;

char gCacheRoot[32] = {};
std::atomic<bool> gConfigured{false};
std::atomic<TaskHandle_t> gForegroundTask{nullptr};

std::mutex gActiveMutex;  // guards the two strings below
std::string gActiveKey;
std::string gActiveSource;

std::atomic<bool> gPassBusy{false};
std::atomic<bool> gBackgroundPending{false};
std::atomic<bool> gBootSweepArmed{false};
std::atomic<bool> gBootSweepDue{false};
// Bumped on every book open.  Once a low-space pass found nothing to evict,
// failed writes stop asking until another book is opened (the previous one
// then becomes evictable).
std::atomic<uint32_t> gOpenEpoch{1};
std::atomic<uint32_t> gExhaustedEpoch{0};

std::atomic<size_t> gTotalBytes{0};
std::atomic<size_t> gLastFreeBytes{0};
std::atomic<uint32_t> gLastFreeCheckMs{0};
std::atomic<bool> gHaveFreeCheck{false};

class PassGuard {
 public:
  PassGuard() {
    bool expected = false;
    held_ = gPassBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
  }
  ~PassGuard() {
    if (held_) gPassBusy.store(false, std::memory_order_release);
  }
  PassGuard(const PassGuard&) = delete;
  PassGuard& operator=(const PassGuard&) = delete;
  explicit operator bool() const { return held_; }

 private:
  bool held_ = false;
};

std::string cacheRoot() { return std::string(gCacheRoot); }

bool isActiveBook(const std::string& name) {
  std::lock_guard<std::mutex> lock(gActiveMutex);
  return dirNameHasKey(name, gActiveKey);
}

bool evictionExhausted() {
  return gExhaustedEpoch.load(std::memory_order_acquire) == gOpenEpoch.load(std::memory_order_acquire);
}

// After a pass: below the reserve with nothing removed means only the open
// book is left (or what remains cannot be removed).
void noteExhaustedIfNothingLeft(const PassResult& result) {
  if (result.freeAfter < kReserveFreeBytes && result.orphansRemoved + result.lruRemoved + result.failed == 0 &&
      !result.aborted) {
    gExhaustedEpoch.store(gOpenEpoch.load(std::memory_order_acquire), std::memory_order_release);
  }
}

std::string baseName(const String& path) {
  const int slash = path.lastIndexOf('/');
  return std::string(slash < 0 ? path.c_str() : path.c_str() + slash + 1);
}

size_t measureFreeBytes() {
  size_t total = gTotalBytes.load(std::memory_order_relaxed);
  if (total == 0) {
    total = LittleFS.totalBytes();  // the partition size never changes
    gTotalBytes.store(total, std::memory_order_relaxed);
  }
  // An info failure reads as "plenty": evicting on a bogus measurement would
  // wipe every cache.
  if (total == 0) return SIZE_MAX;
  const size_t used = LittleFS.usedBytes();
  const size_t freeBytes = used < total ? total - used : 0;
  gLastFreeBytes.store(freeBytes, std::memory_order_relaxed);
  gLastFreeCheckMs.store(millis(), std::memory_order_relaxed);
  gHaveFreeCheck.store(true, std::memory_order_release);
  return freeBytes;
}

bool recentFreeCheckShowedRoom() {
  return gHaveFreeCheck.load(std::memory_order_acquire) &&
         static_cast<uint32_t>(millis() - gLastFreeCheckMs.load(std::memory_order_relaxed)) < kFreeCheckReuseMs &&
         gLastFreeBytes.load(std::memory_order_relaxed) >= kReserveFreeBytes;
}

bool readRecord(const std::string& path, uint32_t* seq, std::string* source) {
  // exists() first: opening a missing file logs a VFS error.
  if (!LittleFS.exists(path.c_str())) return false;
  File file = LittleFS.open(path.c_str(), "r");
  if (!file) return false;
  const size_t size = file.size();
  uint8_t buffer[kMaxRecordBytes];
  const size_t got = size <= sizeof(buffer) ? file.read(buffer, size) : 0;
  file.close();
  return got == size && decodeOpenRecord(buffer, size, seq, source);
}

bool writeRecord(const std::string& path, const uint32_t seq, const std::string& source) {
  const std::vector<uint8_t> bytes = encodeOpenRecord(seq, source);
  // littlefs commits a file on close: a power cut leaves the previous record.
  File file = LittleFS.open(path.c_str(), "w");
  if (!file) return false;
  const size_t written = file.write(bytes.data(), bytes.size());
  file.close();
  return written == bytes.size();
}

// Calls `visit(fullPath, name)` for each book-named directory directly under
// `root`.
template <typename Visit>
void forEachBookDir(const std::string& root, Visit&& visit) {
  if (!LittleFS.exists(root.c_str())) return;
  File dir = LittleFS.open(root.c_str(), "r");
  if (!dir) return;
  if (dir.isDirectory()) {
    for (;;) {
      bool isDir = false;
      const String entry = dir.getNextFileName(&isDir);
      if (entry.length() == 0) break;
      if (!isDir) continue;
      const std::string name = baseName(entry);
      if (isBookCacheDirName(name)) visit(std::string(entry.c_str()), name);
    }
  }
  dir.close();
}

uint32_t maxRecordedSeq() {
  uint32_t maxSeq = 0;
  forEachBookDir(cacheRoot(), [&maxSeq](const std::string& path, const std::string&) {
    uint32_t seq = 0;
    if (readRecord(path + "/" + kOpenRecordFile, &seq, nullptr) && seq > maxSeq) maxSeq = seq;
  });
  return maxSeq;
}

// Removes a tree, checking `shouldAbort` before each entry.  A littlefs
// directory being read while entries are removed may skip one, so a directory
// that is not empty afterwards is walked again.
bool removeTree(const std::string& path, const std::function<bool()>& shouldAbort, const int depth,
                uint32_t* filesRemoved) {
  if (depth > kMaxTreeDepth) return false;
  if (!LittleFS.exists(path.c_str())) return true;
  for (int round = 0; round < 3; ++round) {
    File dir = LittleFS.open(path.c_str(), "r");
    if (!dir) return false;
    if (!dir.isDirectory()) {
      dir.close();
      const bool removed = LittleFS.remove(path.c_str());
      if (removed && filesRemoved) ++*filesRemoved;
      return removed;
    }
    bool complete = true;
    for (;;) {
      if (shouldAbort && shouldAbort()) {
        dir.close();
        return false;
      }
      bool isDir = false;
      const String entry = dir.getNextFileName(&isDir);
      if (entry.length() == 0) break;
      if (isDir) {
        if (!removeTree(entry.c_str(), shouldAbort, depth + 1, filesRemoved)) complete = false;
      } else if (LittleFS.remove(entry.c_str())) {
        if (filesRemoved) ++*filesRemoved;
      } else {
        complete = false;
      }
    }
    dir.close();
    if (LittleFS.rmdir(path.c_str())) return true;
    if (!complete) return false;  // a file is held open: another round would fail the same way
  }
  return false;
}

class LittleFsTarget final : public Target {
 public:
  // `knownFree`: a measurement the caller just made (SIZE_MAX: none), used
  // for the pass's first reading instead of walking the filesystem again.
  LittleFsTarget(const std::function<bool()>& shouldAbort, const size_t knownFree)
      : shouldAbort_(shouldAbort), knownFree_(knownFree) {}

  size_t freeBytes() override {
    if (knownFree_ != SIZE_MAX) {
      const size_t known = knownFree_;
      knownFree_ = SIZE_MAX;
      return known;
    }
    return measureFreeBytes();
  }

  bool removeBook(const Candidate& candidate) override {
    // The candidates were scanned before a book open that may have happened
    // since (a pass outliving its reader session).
    if (isActiveBook(candidate.name)) return false;
    const uint32_t startedMs = millis();
    uint32_t files = 0;
    bool ok = true;
    const std::string cacheDir = cacheRoot() + "/" + candidate.name;
    if (LittleFS.exists(cacheDir.c_str())) {
      LittleFS.remove((cacheDir + "/" + kSourceFingerprintFile).c_str());
      LittleFS.remove((cacheDir + "/" + kOpenRecordFile).c_str());
      ok = removeTree(cacheDir, shouldAbort_, 0, &files);
    }
    const std::string imageDir = std::string(kImageRoot) + "/" + candidate.name;
    if (LittleFS.exists(imageDir.c_str())) ok = removeTree(imageDir, shouldAbort_, 0, &files) && ok;
    LOG_INF(TAG, "[CACHE-EVICT] %s %s seq=%u files=%u%s (%u ms)", candidate.orphan ? "orphan" : "evict",
            candidate.name.c_str(), static_cast<unsigned>(candidate.openSeq), static_cast<unsigned>(files),
            ok ? "" : " INCOMPLETE", static_cast<unsigned>(millis() - startedMs));
    return ok;
  }

 private:
  const std::function<bool()>& shouldAbort_;
  size_t knownFree_;
};

std::vector<Candidate> scanCandidates(const bool checkOrphans) {
  std::string activeKey;
  std::string activeSource;
  {
    std::lock_guard<std::mutex> lock(gActiveMutex);
    activeKey = gActiveKey;
    activeSource = gActiveSource;
  }

  std::vector<Candidate> candidates;
  std::vector<std::string> sources;
  forEachBookDir(cacheRoot(), [&](const std::string& path, const std::string& name) {
    Candidate candidate;
    candidate.name = name;
    candidate.isProtected = dirNameHasKey(name, activeKey);
    std::string source;
    (void)readRecord(path + "/" + kOpenRecordFile, &candidate.openSeq, &source);
    candidates.push_back(std::move(candidate));
    sources.push_back(std::move(source));
  });

  const size_t cacheDirs = candidates.size();
  forEachBookDir(kImageRoot, [&](const std::string&, const std::string& name) {
    for (size_t i = 0; i < cacheDirs; ++i) {
      if (candidates[i].name == name) return;
    }
    // Its cache dir is gone: nothing reads these BMPs any more.
    Candidate candidate;
    candidate.name = name;
    candidate.isProtected = dirNameHasKey(name, activeKey);
    candidate.orphan = true;
    candidates.push_back(std::move(candidate));
  });

  // Only trust "missing on SD" while the card demonstrably serves the open
  // book; an unmounted or flaky card would otherwise orphan every cache.
  if (checkOrphans && SdMan.ready() && !activeSource.empty() && SdMan.exists(activeSource.c_str())) {
    for (size_t i = 0; i < cacheDirs; ++i) {
      if (!candidates[i].isProtected && !sources[i].empty() && !SdMan.exists(sources[i].c_str())) {
        candidates[i].orphan = true;
      }
    }
  }
  return candidates;
}

// Returns false (and leaves `out` untouched) when another pass is running.
bool runLoggedPass(const char* reason, const bool checkOrphans, const size_t knownFree,
                   const std::function<bool()>& shouldAbort, PassResult* out) {
  const PassGuard guard;
  if (!guard) return false;
  const uint32_t startedMs = millis();
  const std::vector<Candidate> candidates = scanCandidates(checkOrphans);
  const uint32_t scanMs = millis() - startedMs;
  LittleFsTarget target(shouldAbort, knownFree);
  const PassResult result = runPass(candidates, target, kReserveFreeBytes, kTargetFreeBytes, shouldAbort);
  LOG_INF(TAG,
          "[CACHE-EVICT] pass reason=%s free=%u->%u KB freed=%d KB removed orphan=%u lru=%u failed=%u aborted=%u "
          "books=%u scan=%u ms total=%u ms",
          reason, static_cast<unsigned>(result.freeBefore / 1024), static_cast<unsigned>(result.freeAfter / 1024),
          static_cast<int>((static_cast<long long>(result.freeAfter) - static_cast<long long>(result.freeBefore)) /
                           1024),
          static_cast<unsigned>(result.orphansRemoved), static_cast<unsigned>(result.lruRemoved),
          static_cast<unsigned>(result.failed), static_cast<unsigned>(result.aborted),
          static_cast<unsigned>(candidates.size()), static_cast<unsigned>(scanMs),
          static_cast<unsigned>(millis() - startedMs));
  (void)scanMs;
  if (out) *out = result;
  return true;
}

bool onCacheWriteFailed(const char* failedPath) {
  if (!gConfigured.load(std::memory_order_acquire)) return false;
  if (evictionExhausted() || recentFreeCheckShowedRoom()) return false;

  if (xTaskGetCurrentTaskHandle() == gForegroundTask.load(std::memory_order_acquire)) {
    // Measuring and deleting would stall the UI loop: the reader's worker
    // decides whether space is the problem.
    if (!gBackgroundPending.exchange(true, std::memory_order_acq_rel)) {
      LOG_INF(TAG, "[CACHE-EVICT] write failed on the UI task (%s): background pass requested",
              failedPath ? failedPath : "?");
    }
    return false;
  }

  if (gPassBusy.load(std::memory_order_acquire)) return false;
  const size_t freeBytes = measureFreeBytes();
  if (freeBytes >= kReserveFreeBytes) return false;
  LOG_INF(TAG, "[CACHE-EVICT] write failed with %u KB free: %s", static_cast<unsigned>(freeBytes / 1024),
          failedPath ? failedPath : "?");

  const uint32_t startedMs = millis();
  const std::function<bool()> budgetSpent = [startedMs]() {
    return static_cast<uint32_t>(millis() - startedMs) >= kWriteFailurePassBudgetMs;
  };
  PassResult result;
  if (!runLoggedPass("write-failure", /*checkOrphans=*/false, freeBytes, budgetSpent, &result)) return false;
  noteExhaustedIfNothingLeft(result);
  // Still short (the budget ran out): the worker continues in the background.
  if (result.freeAfter < kReserveFreeBytes && !evictionExhausted()) {
    gBackgroundPending.store(true, std::memory_order_release);
  }
  return result.freeAfter > result.freeBefore;
}

}  // namespace

void beforeBookOpen(const char* cacheRoot, const std::string& sourcePath) {
  if (!gConfigured.load(std::memory_order_acquire)) {
    if (!cacheRoot || std::strlen(cacheRoot) >= sizeof(gCacheRoot)) return;
    std::strncpy(gCacheRoot, cacheRoot, sizeof(gCacheRoot) - 1);
    gConfigured.store(true, std::memory_order_release);
    setWriteFailureHandler(&onCacheWriteFailed);
  }
  gForegroundTask.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(gActiveMutex);
    gActiveKey = bookCacheKey(sourcePath);
    gActiveSource = sourcePath;
  }
  gOpenEpoch.fetch_add(1, std::memory_order_acq_rel);
  // Once per boot the reader's worker sweeps out caches of deleted books.
  if (!gBootSweepArmed.exchange(true, std::memory_order_acq_rel)) {
    gBootSweepDue.store(true, std::memory_order_release);
    gBackgroundPending.store(true, std::memory_order_release);
  }

  const uint32_t startedMs = millis();
  const size_t freeBytes = measureFreeBytes();
  LOG_INF(TAG, "[PERF] cache free=%u KB reserve=%u KB (%u ms)", static_cast<unsigned>(freeBytes / 1024),
          static_cast<unsigned>(kReserveFreeBytes / 1024), static_cast<unsigned>(millis() - startedMs));
  (void)startedMs;
  if (freeBytes >= kReserveFreeBytes) return;
  (void)runLoggedPass("book-open", /*checkOrphans=*/true, freeBytes, {}, nullptr);
}

void afterBookOpened(const std::string& sourcePath, const std::string& bookCacheDir) {
  if (!gConfigured.load(std::memory_order_acquire) || bookCacheDir.empty()) return;
  const std::string counterPath = cacheRoot() + "/" + kOpenCounterFile;
  const std::string recordPath = bookCacheDir + "/" + kOpenRecordFile;

  uint32_t counter = 0;
  const bool haveCounter = readRecord(counterPath, &counter, nullptr);
  uint32_t current = 0;
  std::string recordedSource;
  const bool haveRecord = readRecord(recordPath, &current, &recordedSource);
  // Re-opening the most recent book (wake, back from Home) changes nothing.
  if (haveCounter && haveRecord && current == counter && recordedSource == sourcePath) return;

  if (!haveCounter) {
    // First boot with eviction, or a lost counter: continue after the newest
    // record so this book does not sort as the oldest.
    counter = maxRecordedSeq();
    LOG_INF(TAG, "[CACHE-EVICT] open counter missing; resuming after seq=%u", static_cast<unsigned>(counter));
  }
  const uint32_t seq = counter == UINT32_MAX ? counter : counter + 1;
  // Counter first: a power cut between the writes only skips a number.
  const bool counterOk = writeRecord(counterPath, seq, std::string());
  const bool recordOk = writeRecord(recordPath, seq, sourcePath);
  if (!counterOk || !recordOk) {
    LOG_ERR(TAG, "[CACHE-EVICT] open record write failed (counter=%u record=%u) %s", static_cast<unsigned>(counterOk),
            static_cast<unsigned>(recordOk), recordPath.c_str());
  } else {
    LOG_DBG(TAG, "[CACHE-EVICT] opened seq=%u %s", static_cast<unsigned>(seq), bookCacheDir.c_str());
  }
}

bool backgroundPassPending() {
  return gConfigured.load(std::memory_order_acquire) && gBackgroundPending.load(std::memory_order_acquire);
}

void runBackgroundPass(const std::function<bool()>& shouldAbort) {
  if (!gBackgroundPending.exchange(false, std::memory_order_acq_rel)) return;
  const bool sweepDue = gBootSweepDue.load(std::memory_order_acquire);
  // A UI-task write failure only asked for a look: without the boot sweep and
  // with room to spare, skip the scan.
  const size_t freeBytes = measureFreeBytes();
  if (!sweepDue && freeBytes >= kReserveFreeBytes) return;
  PassResult result;
  if (!runLoggedPass("background", /*checkOrphans=*/true, freeBytes, shouldAbort, &result) || result.aborted) {
    gBackgroundPending.store(true, std::memory_order_release);  // still due
    return;
  }
  gBootSweepDue.store(false, std::memory_order_release);
  noteExhaustedIfNothingLeft(result);
}

}  // namespace cache_fs::eviction
