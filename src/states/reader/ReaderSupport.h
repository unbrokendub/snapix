#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace papyrix::reader {

struct HeapState {
  size_t freeBytes = 0;
  size_t largestBlock = 0;
};

constexpr int kCacheTaskStackSize = 20480;
constexpr int kCacheTaskStopTimeoutMs = 10000;
constexpr uint8_t kPendingTocJumpMaxRetries = 3;
constexpr uint8_t kPendingEpubPageLoadMaxRetries = 3;
constexpr int kInteractiveCacheTaskPriority = 2;
constexpr uint8_t kEpubActivePrefetchAheadSpines = 3;
constexpr uint32_t kEpubDeepIdleSweepDelayMs = 8000;
constexpr uint32_t kIdleBackgroundKickIntervalMs = 1500;
constexpr uint32_t kEpubActivePartialWorkerCooldownMs = 1200;
constexpr uint32_t kPendingPageLoadOverlayDelayMs = 650;
constexpr size_t kPrefetchRetryFreeHeadroomBytes = 24 * 1024;
constexpr size_t kPrefetchRetryLargestHeadroomBytes = 12 * 1024;
constexpr uint16_t kDefaultCacheBatchPages = 5;
constexpr uint16_t kEpubInteractiveHotExtendBatchPages = 2;
constexpr uint16_t kEpubInteractivePageFillHeadroomPages = 2;
constexpr uint16_t kNonResumableCacheBatchPages = 10;
constexpr int kHorizontalPadding = 5;
constexpr int kStatusBarMargin = 23;

uint32_t perfMsNow();
void perfLog(const char* origin, const char* phase, uint32_t startedMs, const char* fmt = nullptr, ...);

std::string epubSectionCachePath(const std::string& epubCachePath, int spineIndex);
std::string fb2SectionCachePath(const std::string& fb2CachePath, int fontId, int tocIndex);
std::string contentCachePath(const char* cacheDir, int fontId);

HeapState readHeapState();
bool isHeapCritical(const HeapState& heap);
bool isHeapTight(const HeapState& heap);

}  // namespace papyrix::reader
