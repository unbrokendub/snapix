#pragma once

// v3.11.12 — heap low-water localisation.  The main loop only reports the
// cumulative "Min Free" every few seconds, which says THAT the heap nearly ran
// out, not WHERE.  Drop memCheckpoint("tag") after heavy steps: when the
// low-water mark has fallen by more than 2 KB since the previous checkpoint,
// one line names the step that just finished (the dip happened inside it or
// in work running concurrently with it).

#include <cstdint>

#if defined(ARDUINO)
#include <esp_heap_caps.h>

#include "Logging.h"

inline void memCheckpoint(const char* where) {
  static uint32_t lastReported = UINT32_MAX;
  const uint32_t minFree = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  if (lastReported != UINT32_MAX && minFree + 2048 >= lastReported) return;
  lastReported = minFree;
  LOG_INF("MEM", "low-water %u B after %s (free=%u largest=%u)", static_cast<unsigned>(minFree), where,
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}
#else
inline void memCheckpoint(const char*) {}
#endif
