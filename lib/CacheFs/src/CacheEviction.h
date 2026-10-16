#pragma once

// v3.11.12 — automatic least-recently-opened eviction of per-book caches on
// the internal LittleFS partition (policy: CacheEvictionPolicy.h).
//
// When it runs:
//   * reader entry, before the book is opened (UI task, under "Loading..."):
//     a pass if free space is below the reserve, so the open and its first
//     cache build have room;
//   * a cache write that failed on a background task with free space below the
//     reserve (CacheSpaceHook.h): a time-boxed pass, then the write retries;
//   * the reader's background worker (runBackgroundPass): a pass a UI-task
//     write failure asked for, the remainder of a time-boxed one, and once per
//     boot an orphan sweep (caches whose book is gone from the SD card).
// Never during a page turn on the UI loop.  The open book's cache is never
// evicted, nor anything outside per-book dirs ("/cache/<fmt>_<hash>" and
// Fb2's "/img/<fmt>_<hash>").

#include <functional>
#include <string>

namespace cache_fs::eviction {

// Reader entry (UI task), before the content is opened.  Makes `sourcePath`'s
// cache the protected one and evicts if free space is below the reserve.
void beforeBookOpen(const char* cacheRoot, const std::string& sourcePath);

// After a successful open: stamps `bookCacheDir` with the next open sequence
// number (skipped when it already holds the latest one).
void afterBookOpened(const std::string& sourcePath, const std::string& bookCacheDir);

// Background worker: a deferred pass is due.
bool backgroundPassPending();
// Runs it, honouring `shouldAbort` between files; an aborted pass stays due.
void runBackgroundPass(const std::function<bool()>& shouldAbort);

}  // namespace cache_fs::eviction
