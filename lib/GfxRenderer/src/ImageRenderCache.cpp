#include "ImageRenderCache.h"

#include <Logging.h>
#include <esp_heap_caps.h>

#define TAG "IMG_CACHE"

const ImageRenderCache::Entry* ImageRenderCache::get(const std::string& path) {
  auto it = entries_.find(path);
  if (it == entries_.end()) return nullptr;
  // Bump LRU seq.  Wraparound at 2^32 is fine — eviction picks the smallest
  // value, and any wraparound just means a newly-touched entry briefly
  // looks "old" until the next get() bumps it again.
  it->second.lastUsedSeq = ++seqCounter_;
  return &it->second;
}

void ImageRenderCache::put(const std::string& path, std::unique_ptr<uint8_t[]> data, size_t size) {
  if (!data || size == 0) return;

  // Heap-pressure guard: skip the put when free heap is below 60 KB OR the
  // largest contiguous block can't even hold this entry plus a comfortable
  // working margin.  Pinning a 30 KB cache buffer when the BG image decode
  // worker needs ~50 KB of contiguous heap (JPEGDEC workspace + scratch)
  // is what tipped v2.0.51 into the permanent-lock state — once heap
  // dropped below `isHeapCritical` thresholds (free < 28 KB / largest <
  // 10 KB) the worker couldn't run, the cache couldn't shrink (no
  // eviction trigger), and image decodes stalled forever.  Skipping the
  // put here leaves heap freer for the worker even at the cost of one
  // extra SD slurp per render.
  const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestNow = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeNow < 60 * 1024 || largestNow < (size + 12 * 1024)) {
    LOG_DBG(TAG, "skip put %s (size=%u, free=%u, largest=%u)", path.c_str(),
            static_cast<unsigned>(size), static_cast<unsigned>(freeNow),
            static_cast<unsigned>(largestNow));
    return;
  }

  // Replace existing entry: subtract its bytes from total before overwriting.
  auto it = entries_.find(path);
  if (it != entries_.end()) {
    totalBytes_ -= it->second.size;
    it->second.data = std::move(data);
    it->second.size = size;
    it->second.lastUsedSeq = ++seqCounter_;
    totalBytes_ += size;
  } else {
    Entry e;
    e.data = std::move(data);
    e.size = size;
    e.lastUsedSeq = ++seqCounter_;
    entries_.emplace(path, std::move(e));
    totalBytes_ += size;
  }

  evictLruIfOverBudget();
}

void ImageRenderCache::invalidate(const std::string& path) {
  auto it = entries_.find(path);
  if (it == entries_.end()) return;
  totalBytes_ -= it->second.size;
  entries_.erase(it);
}

void ImageRenderCache::clear() {
  entries_.clear();
  totalBytes_ = 0;
  seqCounter_ = 0;
}

void ImageRenderCache::evictLruIfOverBudget() {
  while (totalBytes_ > budgetBytes_ && !entries_.empty()) {
    // Linear scan for oldest entry.  N is tiny (typically < 10), so O(N)
    // beats maintaining a separate ordered structure.
    auto oldest = entries_.begin();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->second.lastUsedSeq < oldest->second.lastUsedSeq) {
        oldest = it;
      }
    }
    LOG_DBG(TAG, "evict %s (%u bytes, total %u/%u)", oldest->first.c_str(),
            static_cast<unsigned>(oldest->second.size), static_cast<unsigned>(totalBytes_),
            static_cast<unsigned>(budgetBytes_));
    totalBytes_ -= oldest->second.size;
    entries_.erase(oldest);
  }
}
