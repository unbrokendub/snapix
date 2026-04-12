#include "ReaderSupport.h"

#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cstdarg>
#include <cstdio>

namespace papyrix::reader {

namespace {
#ifndef PAPYRIX_PERF_LOG
#define PAPYRIX_PERF_LOG 0
#endif
}  // namespace

uint32_t perfMsNow() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

void perfLog(const char* origin, const char* phase, const uint32_t startedMs, const char* fmt, ...) {
#if PAPYRIX_PERF_LOG
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

bool isHeapCritical(const HeapState& heap) { return heap.freeBytes < 72 * 1024 || heap.largestBlock < 36 * 1024; }

bool isHeapTight(const HeapState& heap) { return heap.freeBytes < 96 * 1024 || heap.largestBlock < 52 * 1024; }

}  // namespace papyrix::reader
