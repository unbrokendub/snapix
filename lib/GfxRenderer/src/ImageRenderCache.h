#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

// ============================================================================
// ImageRenderCache
//
// Tiny LRU of decoded BMP files held in RAM, keyed by absolute disk path.
// Lets `ImageBlock::render` hit the BMP without paying the post-write-recovery
// SD-read latency cliff (typically ~150 ms per cache miss on a write-loaded
// card).  After the BG worker decodes a JPEG via `Fb2::decodeImageDirect`,
// the resulting BMP is on disk; on first render we slurp it once into the
// cache and every subsequent render — including the AA pipeline's repeat
// passes — reads from RAM.
//
// Why path-keyed (not binaryId-keyed): keeps the cache GfxRenderer-level so
// ALL content types (FB2 inline figures, EPUB cover, TXT thumbnail) benefit
// without the cache having to know about content-type-specific identifiers.
// `ImageBlock::cachedBmpPath` is already the absolute path; we just memoize
// `parseAndLoadAll` on top of it.
//
// Eviction: LRU.  Soft budget defaults to 96 KB total — enough for two
// 452×600 BMPs (~34 KB each) plus headroom, well under the ~80 KB free
// heap we typically have on ESP32-C3 mid-render.
// ============================================================================
class ImageRenderCache {
 public:
  struct Entry {
    std::unique_ptr<uint8_t[]> data;
    size_t size = 0;
    uint32_t lastUsedSeq = 0;  // monotonic counter for LRU ordering
  };

  ImageRenderCache() = default;
  ~ImageRenderCache() = default;

  ImageRenderCache(const ImageRenderCache&) = delete;
  ImageRenderCache& operator=(const ImageRenderCache&) = delete;

  // Look up a cached BMP by its absolute disk path.  Returns nullptr on
  // miss; on hit returns a pointer + size that remain valid until the next
  // `put` call (or `clear`/`invalidate`).  Updates LRU recency.
  const Entry* get(const std::string& path);

  // Insert (or replace) the BMP for `path`.  `data` is moved into the cache.
  // Triggers LRU eviction if total bytes exceed the soft budget.
  void put(const std::string& path, std::unique_ptr<uint8_t[]> data, size_t size);

  // Drop one specific entry (used when the underlying file is rewritten —
  // e.g. after BG worker promotes preview to full).
  void invalidate(const std::string& path);

  // Drop everything — called on book switch.
  void clear();

  // Soft budget: when totalBytes_ exceeds this, evict LRU until back under.
  // Default 32 KB — enough for one mid-sized BMP without pinning heap to
  // the point where BG worker's `isHeapCritical` watchdog (free < 28 KB OR
  // largest < 10 KB) trips and deadlocks the decode pipeline.  v2.0.51 had
  // 96 KB which left the worker permanently locked out once 2-3 images were
  // cached.  Caller can override via setBudget().
  void setBudget(size_t bytes) { budgetBytes_ = bytes; }
  size_t budget() const { return budgetBytes_; }
  size_t totalBytes() const { return totalBytes_; }
  size_t entryCount() const { return entries_.size(); }

 private:
  void evictLruIfOverBudget();

  std::unordered_map<std::string, Entry> entries_;
  size_t totalBytes_ = 0;
  size_t budgetBytes_ = 32 * 1024;  // 32 KB — see above
  uint32_t seqCounter_ = 0;
};
