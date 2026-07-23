#include "PendingImageDecode.h"

#include <ImageConverter.h>
#include <LittleFS.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <vector>

#define TAG "ASYNC_IMG"

namespace snapix::pendingImage {
namespace {

constexpr size_t kMaxQueueSize = 32;

SemaphoreHandle_t& mutex() {
  static SemaphoreHandle_t m = []() {
    SemaphoreHandle_t handle = xSemaphoreCreateMutex();
    return handle;
  }();
  return m;
}

std::deque<PendingImageDecode>& queue() {
  static std::deque<PendingImageDecode> q;
  return q;
}

std::vector<std::string>& activeTargets() {
  static std::vector<std::string> targets;
  return targets;
}

std::atomic<bool> gRefreshPending{false};

class ScopedLock {
 public:
  ScopedLock() : owned_(false) {
    SemaphoreHandle_t m = mutex();
    if (m && xSemaphoreTake(m, pdMS_TO_TICKS(2000)) == pdTRUE) {
      owned_ = true;
    }
  }
  ~ScopedLock() {
    if (owned_) xSemaphoreGive(mutex());
  }
  ScopedLock(const ScopedLock&) = delete;
  ScopedLock& operator=(const ScopedLock&) = delete;
  explicit operator bool() const { return owned_; }

 private:
  bool owned_;
};

void finishActive(const std::string& targetBmpPath) {
  ScopedLock lock;
  if (lock) {
    auto& active = activeTargets();
    active.erase(std::remove(active.begin(), active.end(), targetBmpPath),
                 active.end());
  }
}

bool decodeItem(PendingImageDecode item,
                const std::function<bool()>& shouldAbort) {
  const std::string activePath = item.targetBmpPath;
  auto finish = [&activePath]() { finishActive(activePath); };

  // If the BMP already exists (could happen if a sync decode won a race),
  // skip and clean up.
  if (LittleFS.exists(item.targetBmpPath.c_str())) {
    LittleFS.remove(item.tempJpegPath.c_str());
    finish();
    return true;
  }

  // The foreground renderer can enqueue a path-only job so neither EPUB ZIP
  // inflation nor JPEG conversion blocks the visible text page.
  if (!LittleFS.exists(item.tempJpegPath.c_str())) {
    if (!item.prepareInput) {
      LOG_INF(TAG, "drain: temp image missing src=%s — skip",
              item.tempJpegPath.c_str());
      finish();
      return true;
    }
    if (shouldAbort && shouldAbort()) {
      finish();
      return true;
    }
    LOG_INF(TAG, "drain: extract start temp=%s",
            item.tempJpegPath.c_str());
    const uint32_t extractStartedMs = millis();
    if (!item.prepareInput(item.tempJpegPath, shouldAbort)) {
      LittleFS.remove(item.tempJpegPath.c_str());
      LOG_INF(TAG, "drain: extract stopped temp=%s (%u ms)",
              item.tempJpegPath.c_str(),
              static_cast<unsigned>(millis() - extractStartedMs));
      finish();
      return true;
    }
    LOG_INF(TAG, "drain: extract ok temp=%s (%u ms)",
            item.tempJpegPath.c_str(),
            static_cast<unsigned>(millis() - extractStartedMs));
  }

  if (shouldAbort && shouldAbort()) {
    LittleFS.remove(item.tempJpegPath.c_str());
    finish();
    return true;
  }

  ImageConvertConfig config;
  config.maxWidth = item.maxWidth;
  config.maxHeight = item.maxHeight;
  config.quickMode = item.quickMode;
  config.oneBit = item.oneBit;
  config.logTag = item.logTag ? item.logTag : TAG;
  config.outputOnLittleFs = true;
  config.shouldAbort = shouldAbort;

  const std::string partPath = item.targetBmpPath + ".part";
  LittleFS.remove(partPath.c_str());
  LOG_INF(TAG, "drain: convert start temp=%s → %s (%ux%u quick=%u)", item.tempJpegPath.c_str(),
          item.targetBmpPath.c_str(), static_cast<unsigned>(item.maxWidth), static_cast<unsigned>(item.maxHeight),
          static_cast<unsigned>(item.quickMode));
  const uint32_t startMs = millis();
  const bool ok = ImageConverterFactory::convertToBmp(item.tempJpegPath, partPath, config);
  const uint32_t elapsed = millis() - startMs;

  if (ok) {
    LittleFS.remove(item.tempJpegPath.c_str());
    LittleFS.remove(item.targetBmpPath.c_str());
    if (!LittleFS.rename(partPath.c_str(), item.targetBmpPath.c_str())) {
      LOG_ERR(TAG, "drain: rename %s -> %s failed", partPath.c_str(), item.targetBmpPath.c_str());
      LittleFS.remove(partPath.c_str());
      finish();
      return true;
    }
    LOG_INF(TAG, "drain: decode ok %s (%u ms; remaining=%u)", item.targetBmpPath.c_str(),
            static_cast<unsigned>(elapsed), static_cast<unsigned>(pendingCount()));
    gRefreshPending.store(true, std::memory_order_release);
  } else {
    LOG_ERR(TAG, "drain: decode failed %s (%u ms)", item.targetBmpPath.c_str(), static_cast<unsigned>(elapsed));
    LittleFS.remove(item.tempJpegPath.c_str());
    LittleFS.remove(partPath.c_str());
  }

  finish();
  return true;
}

}  // namespace

bool enqueue(PendingImageDecode item, Priority priority) {
  ScopedLock lock;
  if (!lock) {
    LOG_ERR(TAG, "enqueue: failed to acquire lock");
    return false;
  }
  auto& q = queue();
  for (auto it = q.begin(); it != q.end(); ++it) {
    if (it->targetBmpPath != item.targetBmpPath) continue;
    // The existing entry owns its temp input.  Delete only a distinct duplicate
    // temp file; deleting an identical path would strand the queued item.
    if (item.tempJpegPath != it->tempJpegPath) {
      LittleFS.remove(item.tempJpegPath.c_str());
    }
    if (priority == Priority::CurrentPage && it != q.begin()) {
      PendingImageDecode existing = std::move(*it);
      q.erase(it);
      q.push_front(std::move(existing));
    }
    LOG_DBG(TAG, "enqueue: coalesced duplicate target=%s", item.targetBmpPath.c_str());
    return true;
  }
  if (q.size() >= kMaxQueueSize) {
    LOG_INF(TAG, "enqueue: queue full (%u entries) — dropping decode for %s", static_cast<unsigned>(q.size()),
            item.targetBmpPath.c_str());
    // Drop the temp file rather than leave it dangling on LittleFS.
    LittleFS.remove(item.tempJpegPath.c_str());
    return false;
  }
  LOG_INF(TAG, "enqueue: %s → %s (%ux%u, queue=%u)", item.tempJpegPath.c_str(),
          item.targetBmpPath.c_str(), static_cast<unsigned>(item.maxWidth), static_cast<unsigned>(item.maxHeight),
          static_cast<unsigned>(q.size() + 1));
  if (priority == Priority::CurrentPage) {
    q.push_front(std::move(item));
  } else {
    q.push_back(std::move(item));
  }
  return true;
}

bool promote(const std::string& targetBmpPath) {
  ScopedLock lock;
  if (!lock) return false;
  auto& q = queue();
  for (auto it = q.begin(); it != q.end(); ++it) {
    if (it->targetBmpPath != targetBmpPath) continue;
    if (it != q.begin()) {
      PendingImageDecode item = std::move(*it);
      q.erase(it);
      q.push_front(std::move(item));
    }
    return true;
  }
  return false;
}

bool drainTarget(const std::string& targetBmpPath,
                 const std::function<bool()>& shouldAbort) {
  PendingImageDecode item;
  {
    ScopedLock lock;
    if (!lock) return false;
    const auto& active = activeTargets();
    if (std::find(active.begin(), active.end(), targetBmpPath) != active.end()) {
      return true;
    }
    auto& q = queue();
    auto it = std::find_if(
        q.begin(), q.end(), [&targetBmpPath](const PendingImageDecode& candidate) {
          return candidate.targetBmpPath == targetBmpPath;
        });
    if (it == q.end()) return false;
    item = std::move(*it);
    q.erase(it);
    activeTargets().push_back(targetBmpPath);
  }
  return decodeItem(std::move(item), shouldAbort);
}

bool isPendingOrActive(const std::string& targetBmpPath) {
  ScopedLock lock;
  if (!lock) return false;
  const auto& active = activeTargets();
  if (std::find(active.begin(), active.end(), targetBmpPath) != active.end()) {
    return true;
  }
  const auto& q = queue();
  return std::any_of(
      q.begin(), q.end(), [&targetBmpPath](const PendingImageDecode& item) {
        return item.targetBmpPath == targetBmpPath;
      });
}

bool drainOne(const std::function<bool()>& shouldAbort) {
  PendingImageDecode item;
  {
    ScopedLock lock;
    if (!lock) return false;
    auto& q = queue();
    if (q.empty()) return false;
    item = std::move(q.front());
    q.pop_front();
    activeTargets().push_back(item.targetBmpPath);
  }
  return decodeItem(std::move(item), shouldAbort);
}

size_t drainAll(const std::function<bool()>& shouldAbort) {
  size_t done = 0;
  while (drainOne(shouldAbort)) {
    ++done;
    if (shouldAbort && shouldAbort()) break;
  }
  return done;
}

size_t pendingCount() {
  ScopedLock lock;
  if (!lock) return 0;
  return queue().size();
}

bool empty() {
  ScopedLock lock;
  if (!lock) return true;
  return queue().empty();
}

size_t purgePrefix(const std::string& bmpPathPrefix) {
  ScopedLock lock;
  if (!lock) return 0;
  auto& q = queue();
  size_t removed = 0;
  for (auto it = q.begin(); it != q.end();) {
    if (it->targetBmpPath.rfind(bmpPathPrefix, 0) == 0) {
      LittleFS.remove(it->tempJpegPath.c_str());
      it = q.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  if (removed > 0) {
    LOG_INF(TAG, "purgePrefix: %u entries purged for prefix=%s", static_cast<unsigned>(removed),
            bmpPathPrefix.c_str());
  }
  return removed;
}

bool consumeRefreshSignal() {
  return gRefreshPending.exchange(false, std::memory_order_acq_rel);
}

}  // namespace snapix::pendingImage
