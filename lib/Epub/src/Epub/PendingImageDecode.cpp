#include "PendingImageDecode.h"

#include <ImageConverter.h>
#include <LittleFS.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <deque>

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

}  // namespace

bool enqueue(PendingImageDecode item) {
  ScopedLock lock;
  if (!lock) {
    LOG_ERR(TAG, "enqueue: failed to acquire lock");
    return false;
  }
  auto& q = queue();
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
  q.push_back(std::move(item));
  return true;
}

bool drainOne() {
  PendingImageDecode item;
  {
    ScopedLock lock;
    if (!lock) return false;
    auto& q = queue();
    if (q.empty()) return false;
    item = std::move(q.front());
    q.pop_front();
  }

  // Verify the temp file still exists (could have been purged on book close).
  if (!LittleFS.exists(item.tempJpegPath.c_str())) {
    LOG_INF(TAG, "drainOne: temp jpeg missing src=%s — skip", item.tempJpegPath.c_str());
    return true;  // count as "did work" so caller loops to the next entry
  }

  // If the BMP already exists (could happen if a sync decode raced via cache
  // hit before we got here), skip and clean up.
  if (LittleFS.exists(item.targetBmpPath.c_str())) {
    LittleFS.remove(item.tempJpegPath.c_str());
    return true;
  }

  ImageConvertConfig config;
  config.maxWidth = item.maxWidth;
  config.maxHeight = item.maxHeight;
  config.quickMode = item.quickMode;
  config.oneBit = item.oneBit;     // v2.0.88: route through SmolJpeg when true
  config.logTag = item.logTag ? item.logTag : TAG;
  config.outputOnLittleFs = true;

  // v3.10.8 — ATOMIC BMP write.  convertToBmp streams the BMP straight to its
  // output path, so this BG worker used to write directly to targetBmpPath while
  // the FOREGROUND streaming render polls `LittleFS.exists(targetBmpPath)` and
  // reads the file the instant it appears (GfxRendererPaginatorAdapter::drawImage
  // / resolveImageSize).  On the first view of an image page the foreground then
  // read a HALF-WRITTEN BMP → random noise over the image; flipping away and back
  // looked clean only because the file had finished by then.  Decode to a `.part`
  // file and rename on success: the target appears ONLY when complete, so the
  // foreground sees either no file (draws nothing; gRefreshPending repaints once
  // the BMP lands) or the whole file — never a partial one.
  const std::string partPath = item.targetBmpPath + ".part";
  LittleFS.remove(partPath.c_str());  // clear any stale partial from a prior abort/boot
  LOG_INF(TAG, "drainOne: convert start temp=%s → %s (%ux%u quick=%u)", item.tempJpegPath.c_str(),
          item.targetBmpPath.c_str(), static_cast<unsigned>(item.maxWidth), static_cast<unsigned>(item.maxHeight),
          static_cast<unsigned>(item.quickMode));
  const uint32_t startMs = millis();
  const bool ok = ImageConverterFactory::convertToBmp(item.tempJpegPath, partPath, config);
  const uint32_t elapsed = millis() - startMs;

  if (ok) {
    LittleFS.remove(item.tempJpegPath.c_str());
    LittleFS.remove(item.targetBmpPath.c_str());  // drop any stale/invalid prior BMP
    if (!LittleFS.rename(partPath.c_str(), item.targetBmpPath.c_str())) {
      LOG_ERR(TAG, "drainOne: rename %s -> %s failed", partPath.c_str(), item.targetBmpPath.c_str());
      LittleFS.remove(partPath.c_str());
      return true;
    }
    LOG_INF(TAG, "drainOne: decode ok %s (%u ms; remaining=%u)", item.targetBmpPath.c_str(),
            static_cast<unsigned>(elapsed), static_cast<unsigned>(pendingCount()));
    gRefreshPending.store(true, std::memory_order_release);
  } else {
    LOG_ERR(TAG, "drainOne: decode failed %s (%u ms)", item.targetBmpPath.c_str(), static_cast<unsigned>(elapsed));
    LittleFS.remove(item.tempJpegPath.c_str());
    LittleFS.remove(partPath.c_str());  // drop the half-written temp
  }

  return true;
}

size_t drainAll() {
  size_t done = 0;
  while (drainOne()) {
    ++done;
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
