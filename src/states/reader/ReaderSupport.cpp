#include "ReaderSupport.h"

#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cstdarg>
#include <cstdio>

namespace snapix::reader {

namespace {
#ifndef SNAPIX_PERF_LOG
#define SNAPIX_PERF_LOG 0
#endif
}  // namespace

uint32_t perfMsNow() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

void perfLog(const char* origin, const char* phase, const uint32_t startedMs, const char* fmt, ...) {
#if SNAPIX_PERF_LOG
  char suffix[128] = "";
  if (fmt) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(suffix, sizeof(suffix), fmt, args);
    va_end(args);
  }
  LOG_INF(origin, "[PERF] %s: %lu ms%s%s", phase, static_cast<unsigned long>(perfMsNow() - startedMs),
          suffix[0] ? " " : "", suffix);
#else
  (void)origin;
  (void)phase;
  (void)startedMs;
  (void)fmt;
#endif
}

std::string epubSectionCachePath(const std::string& epubCachePath, const int spineIndex) {
  return epubCachePath + "/sections/" + std::to_string(spineIndex) + ".bin";
}

std::string fb2SectionCachePath(const std::string& fb2CachePath, const int fontId, const int tocIndex) {
  return fb2CachePath + "/sections/fb2_" + std::to_string(fontId) + "_" + std::to_string(tocIndex) + ".bin";
}

std::string contentCachePath(const char* cacheDir, const int fontId) {
  return std::string(cacheDir) + "/pages_" + std::to_string(fontId) + ".bin";
}

HeapState readHeapState() {
  return HeapState{heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)};
}

// ESP32-C3 has ~194 KB total heap.  Normal operating free heap during FB2
// reading is 35-85 KB, with the largest contiguous block around 30-35 KB.
// The original thresholds (72 KB free / 36 KB largest) were always exceeded,
// which disabled prefetch entirely and crippled page-turn performance.
//
// Revised thresholds: critical means the device genuinely risks OOM if a
// prefetch or background operation allocates a few KB.
bool isHeapCritical(const HeapState& heap) { return heap.freeBytes < 28 * 1024 || heap.largestBlock < 10 * 1024; }

bool isHeapTight(const HeapState& heap) { return heap.freeBytes < 40 * 1024 || heap.largestBlock < 20 * 1024; }

}  // namespace snapix::reader
