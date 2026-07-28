#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "../../core/EventQueue.h"

namespace snapix::reader {

struct HeapState {
  size_t freeBytes = 0;
  size_t largestBlock = 0;
};

// v2.0.76 hotfix: bumped from 16 KB to 20 KB after a device run reported
// stack high water 15772 bytes during regular FB2 reading — only 612 bytes
// headroom from overflow.  The growth since the original 16 KB sizing comes
// from nested try/catch frames (BG workerLoop + v2.0.74 tryNewUnique +
// JPEG decoder catch) plus deeper recursion in chapter HTML parsing.
// 4 KB extra DRAM is acceptable; stack overflow on this task is silent
// memory corruption and a hard-to-reproduce reset.
//
// v2.0.149: bumped 20480 → 28672 to fix stack-protection panic in
// Fb2Parser R4.c MEASURE walk after StreamingPaginator grew (v2.0.142
// spillover 256→2048, v2.0.145 pending image, v2.0.148 EPUB temp
// parser).
// v2.0.151: REVERTED back to 20480.  The +8 KB ReaderAsync stack +
// the +8 KB loopTask stack bump cost 16 KB of heap, which made
// EPUB chapter prepare (ZIP deflate + HTML normalize, ~30 KB peak)
// fail with "heap dangerously low" on «Гибкий ум» spine 9 — page
// never loaded.  Instead of bumping stack, v2.0.151 heap-allocates
// the 2 KB spilloverBuf_ inside StreamingPaginator, dropping the
// paginator instance from ~2.6 KB to ~600 bytes.  That removes
// the ~2 KB stack overhead in BOTH tasks without spending heap on
// permanent stack reserves.
constexpr int kCacheTaskStackSize = 20480;
// 15s gives the JPEG decoder + FB2 parser their MAX work-then-abort cycle a
// chance to wind down before the nuclear ESP.restart() kicks in.  With the
// per-MCU abort check (every 4 MCUs) added in the JPEG converter, a normal
// abort completes in ~30-200ms; this headroom only matters under genuine
// deadlock or pathological input.
constexpr int kCacheTaskStopTimeoutMs = 15000;
// v2.0.110 (audit fix #1): short wait used by the render-path
// `requestBackgroundCachingPause()`.  Worker's interactive cooperative-
// cancel typically completes within ~100-300 ms (vTaskDelay yield in
// parseLoop + layout callback shouldAbort check).  500 ms covers the
// 99th percentile; longer waits block input polling, which is the
// "buttons unresponsive" symptom.  No ESP.restart() on timeout —
// render proceeds with whatever state the worker left.
constexpr int kInteractiveCacheCancelTimeoutMs = 500;
constexpr uint8_t kPendingTocJumpMaxRetries = 3;
constexpr uint8_t kPendingEpubPageLoadMaxRetries = 3;
// Must not exceed the main Arduino loop priority (1) — on single-core
// ESP32-C3, a higher priority would starve the UI and block button input.
constexpr int kInteractiveCacheTaskPriority = 1;
constexpr uint8_t kEpubActivePrefetchAheadSpines = 3;
constexpr uint32_t kEpubDeepIdleSweepDelayMs = 8000;
constexpr uint32_t kIdleBackgroundKickIntervalMs = 1500;
constexpr uint32_t kEpubActivePartialWorkerCooldownMs = 1200;
constexpr uint32_t kPendingPageLoadOverlayDelayMs = 350;
constexpr size_t kPrefetchRetryFreeHeadroomBytes = 24 * 1024;
constexpr size_t kPrefetchRetryLargestHeadroomBytes = 12 * 1024;
constexpr uint16_t kDefaultCacheBatchPages = 5;
constexpr uint16_t kEpubInteractiveHotExtendBatchPages = 2;
constexpr uint16_t kEpubInteractivePageFillHeadroomPages = 2;
// Once EpubChapterParser has built its page-boundary index, emitted Page
// objects are empty streaming placeholders.  Materialise a distant resume
// target in larger batches to avoid reopening and rewriting the cache LUT
// every five pages.
constexpr uint16_t kEpubIndexedPageFillBatchPages = 50;
constexpr uint16_t kNonResumableCacheBatchPages = 10;
constexpr int kHorizontalPadding = 5;
constexpr int kStatusBarMargin = 23;

// A TOC selection is handled on Center press.  Its matching Center release
// can arrive while the e-ink frame is being rendered and must not be mistaken
// for a new user action: doing so cancels the exact-anchor follow-up as soon as
// page 0 becomes visible.  Page-turn releases remain actionable because page
// navigation is intentionally performed on release.
inline bool cancelsDeferredTocFollowup(const Event& event) {
  if (event.type == EventType::ButtonPress ||
      event.type == EventType::ButtonRepeat) {
    return true;
  }
  if (event.type != EventType::ButtonRelease) {
    return false;
  }
  return event.button == Button::Left || event.button == Button::Right ||
         event.button == Button::Up || event.button == Button::Down ||
         event.button == Button::Power;
}

inline uint32_t inputEventTimeMs(const Event& event, const uint32_t dispatchTimeMs) {
  // timestampMs==0 keeps synthetic and legacy events useful.
  return event.timestampMs != 0 ? event.timestampMs : dispatchTimeMs;
}

inline bool isShortPowerRelease(const Event& releaseEvent, const bool pressActive,
                                const uint32_t pressStartedMs, const uint32_t dispatchTimeMs,
                                const uint32_t shortPressDurationMs) {
  if (!pressActive || releaseEvent.type != EventType::ButtonRelease ||
      releaseEvent.button != Button::Power) {
    return false;
  }
  return static_cast<uint32_t>(inputEventTimeMs(releaseEvent, dispatchTimeMs) - pressStartedMs) <
         shortPressDurationMs;
}

inline bool shouldPrioritizeNextSectionPrefetch(const bool supportsSectionPrefetch,
                                                const bool currentCacheHasPages,
                                                const bool currentCacheNearTail,
                                                const bool nextSectionReady,
                                                const bool readerRecentlyActive,
                                                const bool heapAllowsPrefetch) {
  return supportsSectionPrefetch && currentCacheHasPages && !currentCacheNearTail &&
         !nextSectionReady && !readerRecentlyActive && heapAllowsPrefetch;
}


uint32_t perfMsNow();
void perfLog(const char* origin, const char* phase, uint32_t startedMs, const char* fmt = nullptr, ...);

std::string epubSectionCachePath(const std::string& epubCachePath, int spineIndex);
std::string fb2SectionCachePath(const std::string& fb2CachePath, int fontId, int tocIndex);
std::string contentCachePath(const char* cacheDir, int fontId);

// v2.0.153 — per-book sidecar that caches `globalSectionPageMetrics_` between
// reader sessions.  Replaces the 55× PageCache::probe loop (~200 ms/file →
// ~11 s on Fire_in_the_Valley.fb2) with a single LittleFS read (<5 ms) when
// the configHash matches.  Stored at "<bookCachePath>/metrics.bin".
std::string metricsCachePath(const std::string& bookCachePath);

HeapState readHeapState();
bool isHeapCritical(const HeapState& heap);
bool isHeapTight(const HeapState& heap);
// v2.0.67: hot extend uses an existing parser session and just appends
// pages — its working set is ~5-10 KB transient.  Cold rebuild has its
// own pre-flight check inside PageCache::extend (~25 KB largest /
// 50 KB free).  The "is the heap so tight that even a hot extend will
// OOM" gate can be much looser than isHeapCritical.  Use this in
// background-cache workers that are about to do a hot extend.
bool isHeapCriticalForHotExtend(const HeapState& heap);

}  // namespace snapix::reader
