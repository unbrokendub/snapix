#pragma once

// Internal helpers shared across ReaderState*.cpp translation units.
// Not part of the public API — only included by ReaderState implementation files.

#include <Fb2.h>
#include <Logging.h>
#include <RenderConfig.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <string>

#include "ReaderSupport.h"

#define READER_STATE_TAG "READER"

namespace snapix {

inline bool fb2UsesSectionNavigation(const Fb2Provider* provider) {
  return provider && provider->getFb2() && provider->tocCount() > 0;
}

inline bool resolveFb2SectionContext(const Fb2Provider* provider, const RenderConfig& config, const int tocIndex,
                                     std::string* cachePath, uint32_t* startOffset, int* startingSectionIndex,
                                     uint32_t* endOffset = nullptr) {
  if (!fb2UsesSectionNavigation(provider) || tocIndex < 0 || tocIndex >= static_cast<int>(provider->tocCount())) {
    return false;
  }

  const Fb2::TocItem item = provider->getFb2()->getTocItem(static_cast<uint16_t>(tocIndex));
  if (item.sectionIndex < 0) {
    return false;
  }

  if (cachePath) {
    *cachePath = reader::fb2SectionCachePath(provider->getFb2()->getCachePath(), config.fontId, tocIndex);
  }
  if (startOffset) {
    *startOffset = item.sourceOffset;
  }
  if (startingSectionIndex) {
    *startingSectionIndex = item.sectionIndex;
  }
  if (endOffset) {
    *endOffset = 0;
    if (tocIndex + 1 < static_cast<int>(provider->tocCount())) {
      const Fb2::TocItem nextItem = provider->getFb2()->getTocItem(static_cast<uint16_t>(tocIndex + 1));
      if (nextItem.sectionIndex >= 0 && nextItem.sourceOffset > item.sourceOffset) {
        *endOffset = nextItem.sourceOffset;
      }
    }
  }
  return true;
}

// Local perf logging wrapper that uses the READER tag.
#ifndef SNAPIX_PERF_LOG
#define SNAPIX_PERF_LOG 0
#endif

inline void readerPerfLog(const char* phase, uint32_t startedMs, const char* fmt = nullptr, ...) {
#if SNAPIX_PERF_LOG
  char suffix[128] = "";
  if (fmt) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(suffix, sizeof(suffix), fmt, args);
    va_end(args);
  }
  LOG_INF(READER_STATE_TAG, "[PERF] %s: %lu ms%s%s", phase, static_cast<unsigned long>(reader::perfMsNow() - startedMs),
          suffix[0] ? " " : "", suffix);
#else
  (void)phase;
  (void)startedMs;
  (void)fmt;
#endif
}

// FB2 TOC jump tuning constants — shared across ReaderState*.cpp files.
constexpr int kFb2TocJumpHeadroomPages = 6;
constexpr int kFb2TocJumpMinimumGrowthPages = 24;
constexpr int kFb2TocJumpColdStartCapPages = 72;
constexpr int kFb2TocHintInflationDivisor = 2;

inline int estimateFb2TocTargetPageHint(const Fb2* fb2, const uint32_t estimatedTotalPages,
                                        const Fb2::TocItem& tocItem) {
  if (!fb2 || estimatedTotalPages == 0) {
    return -1;
  }

  const size_t fileSize = fb2->getFileSize();
  if (fileSize == 0) {
    return -1;
  }

  if (tocItem.sourceOffset > 0 && tocItem.sourceOffset < fileSize) {
    const uint64_t scaled = static_cast<uint64_t>(tocItem.sourceOffset) * estimatedTotalPages;
    const int baseHint = static_cast<int>(scaled / fileSize);
    return baseHint + std::max(8, baseHint / kFb2TocHintInflationDivisor);
  }

  return tocItem.sectionIndex >= 0 ? tocItem.sectionIndex : -1;
}

}  // namespace snapix
