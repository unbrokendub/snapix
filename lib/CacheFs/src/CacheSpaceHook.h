#pragma once

// v3.11.12 — "a cache write failed" hook for the LittleFS cache writers
// (UnifiedCache, PageCache, the image decode queue).  Header-only so those
// libraries, and their host tests, need no link dependency on the eviction
// code; CacheEviction.cpp installs the handler on the device.

#include <atomic>

namespace cache_fs {

// Returns true when it freed space, so the caller may retry the write once.
// Cheap when the failure had nothing to do with space; never blocks the UI
// task (there it only schedules a background pass).
using WriteFailureHandler = bool (*)(const char* failedPath);

inline std::atomic<WriteFailureHandler> gWriteFailureHandler{nullptr};

inline void setWriteFailureHandler(const WriteFailureHandler handler) {
  gWriteFailureHandler.store(handler, std::memory_order_release);
}

inline bool reclaimAfterWriteFailure(const char* failedPath) {
  const WriteFailureHandler handler = gWriteFailureHandler.load(std::memory_order_acquire);
  return handler != nullptr && handler(failedPath);
}

}  // namespace cache_fs
