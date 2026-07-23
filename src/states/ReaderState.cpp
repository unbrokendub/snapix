#include "ReaderState.h"

#include <Arduino.h>
#include <ContentParser.h>
#include <CoverHelpers.h>
#include <Epub.h>  // v2.0.153 — Epub::getCachePath for metrics sidecar path
#include <Epub/PendingImageDecode.h>
#include <Epub/parsers/EpubImageCache.h>  // v2.0.148 — cacheImageForStreaming
#include <EpubChapterParser.h>
#include <Fb2.h>
#include <Fb2Parser.h>
#include <FsHelpers.h>  // v2.0.145 — normalisePath for streaming image-resolver
#include <GfxRenderer.h>
#include <JpegToBmpConverter.h>  // v2.0.101: releaseAllPersistent() after cover-gen
#include <LittleFS.h>  // v2.0.153 — metrics.bin sidecar load/save (used in every env)
#include <UnifiedCache.h>  // v2.0.166 — Phase 5 unified streaming cache
#include <Logging.h>
#include <MarkdownParser.h>
#include <Page.h>
#include <PageCache.h>
#include <PlainTextParser.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>

// v2.0.122 Phase R3.5 — streaming render path (opt-in).  Active only in
// builds that flip `SNAPIX_MARKERIZED_RENDER=1`; default builds (and
// even default v3_alpha) compile this section out.  When ON AND a
// markers sidecar exists for the current spine, render goes through
// the v3 streaming pipeline instead of the legacy `page.render()` Page-
// tree path.
//
// v2.0.127 Phase R3.7 adds FB2 support: navigation tocIndex →
// `Fb2::TocItem::sectionIndex` lookup → `markers/<sectionIndex>.bin`.
#if defined(SNAPIX_MARKERIZED_RENDER) && SNAPIX_MARKERIZED_RENDER
#include <Epub.h>
#include <FS.h>
#include <Fb2.h>
#include <ChunkedMarkersReader.h>       // v3.9.0 — chunked (lazy) markers reader
#include <GfxRendererPaginatorAdapter.h>
#include <LittleFS.h>
#include <MarkerizedPageRender.h>
#include <StreamingPaginator.h>
#include <UnifiedCacheChunkProvider.h>  // v3.9.0 — UnifiedCache-backed chunk provider
#endif

#include "../Battery.h"
#include "../FontManager.h"
#include "../config.h"
#include "../content/BookmarkManager.h"
#include "../content/ProgressManager.h"
#include "../content/ReaderNavigation.h"
#include "../core/BootMode.h"
#include "../core/CrashDebug.h"
#include "../core/Core.h"
#include "../ui/Elements.h"
#include "../ui/views/ReaderViews.h"
#include "ThemeManager.h"
#include "reader/ReaderStateInternal.h"
#include "reader/ReaderSupport.h"

#define TAG "READER"

namespace snapix {
using reader::kCacheTaskStopTimeoutMs;
using reader::kIdleBackgroundKickIntervalMs;
using reader::kInteractiveCacheCancelTimeoutMs;

namespace {
constexpr int horizontalPadding = 5;
constexpr int statusBarMargin = 23;
constexpr size_t kEstimatedBytesPerPage = 2048;

uint16_t estimatePagesForBytes(const size_t bytes, const size_t bytesPerPage = kEstimatedBytesPerPage) {
  const size_t safeBytesPerPage = std::max<size_t>(1, bytesPerPage);
  const size_t pageCount = std::max<size_t>(1, (bytes + safeBytesPerPage - 1) / safeBytesPerPage);
  return static_cast<uint16_t>(std::min<size_t>(pageCount, UINT16_MAX));
}

const char* streamContentTypeName(const ContentType type) {
  switch (type) {
    case ContentType::Epub:
      return "epub";
    case ContentType::Fb2:
      return "fb2";
    case ContentType::Txt:
      return "txt";
    case ContentType::Markdown:
      return "markdown";
    case ContentType::Html:
      return "html";
    default:
      return "other";
  }
}

// v2.0.153 — sidecar that caches `globalSectionPageMetrics_` between sessions.
// Replaces 55× PageCache::probe LittleFS opens (~200 ms each on FB2 with TOC
// section navigation) with a single ~400-byte read.  See ReaderSupport.h for
// path layout (`<bookCachePath>/metrics.bin`).
//
// Layout — little-endian, 12-byte header + 7 bytes per spine entry:
//   header:  magic[4]='MTRC', version u8, flags u8, spineCount u16, configHash u32
//   entry:   pages u16, byteSize u32, exact u8
constexpr uint32_t kMetricsCacheMagic = 0x4352544Du;  // 'MTRC' little-endian
constexpr uint8_t kMetricsCacheVersion = 1;
constexpr size_t kMetricsCacheHeaderBytes = 12;
constexpr size_t kMetricsCacheEntryBytes = 7;

// FNV-1a over the RenderConfig fields PageCache::probe validates.  Two configs
// that produce different page boundaries MUST hash to different values.  Hash
// collision in the other direction (cache hit after a real config change) is
// the only failure mode — corrected the next time a section becomes exact and
// overwrites the saved metric.
uint32_t computeMetricsConfigHash(const RenderConfig& config) {
  uint32_t h = 2166136261u;
  auto mixBytes = [&h](const uint8_t* p, const size_t n) {
    for (size_t i = 0; i < n; ++i) {
      h ^= p[i];
      h *= 16777619u;
    }
  };
  auto mixU32 = [&](const uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                          static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    mixBytes(b, 4);
  };
  auto mixU16 = [&](const uint16_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    mixBytes(b, 2);
  };
  uint32_t lineCompressionBits = 0;
  std::memcpy(&lineCompressionBits, &config.lineCompression, sizeof(uint32_t));
  mixU32(static_cast<uint32_t>(config.fontId));
  mixU32(lineCompressionBits);
  mixBytes(&config.indentLevel, 1);
  mixBytes(&config.spacingLevel, 1);
  mixBytes(&config.paragraphAlignment, 1);
  const uint8_t boolPack = (config.hyphenation ? 1 : 0) | (config.showImages ? 2 : 0) |
                           (config.bionicReading ? 4 : 0);
  mixBytes(&boolPack, 1);
  mixBytes(&config.fakeBold, 1);
  mixU16(config.viewportWidth);
  mixU16(config.viewportHeight);
  // Format tag — bump if the on-disk layout ever changes so old files with
  // a valid configHash but wrong byte layout are rejected.
  const uint8_t formatTag = kMetricsCacheVersion;
  mixBytes(&formatTag, 1);
  return h;
}

// Resolves the per-book cache path the metrics sidecar lives under.  Returns
// empty string for content types we don't paginate at the section-metric
// level (TXT, MD, plain HTML, XTC) or when the content object isn't loaded.
std::string bookCachePathForMetrics(snapix::Core& core) {
  const ContentType type = core.content.metadata().type;
  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return {};
    return provider->getEpub()->getCachePath();
  }
  if (type == ContentType::Fb2) {
    auto* provider = core.content.asFb2();
    if (!provider || !provider->getFb2()) return {};
    return provider->getFb2()->getCachePath();
  }
  return {};
}
}  // namespace

void ReaderState::clearPagePrefetch() {
  cacheController_.clearPagePrefetch();
}

void ReaderState::clearLookaheadParser() {
  cacheController_.clearLookaheadParser();
}

void ReaderState::resetBackgroundPrefetchState() { cacheController_.resetBackgroundPrefetchState(); }

const char* ReaderState::backgroundCacheWakeReasonToString(const BackgroundCacheWakeReason reason) {
  return reader::ReaderCacheController::backgroundCacheWakeReasonToString(reason);
}

bool ReaderState::promoteLookaheadParser(const int targetSpine) {
  const bool promoted = cacheController_.promoteLookaheadParser(targetSpine);
  if (promoted) {
    LOG_DBG(TAG, "Promoted lookahead EPUB parser spine=%d", targetSpine);
  }
  return promoted;
}

void ReaderState::prefetchAdjacentPage(Core& core) { cacheController_.prefetchAdjacentPage(core); }

void ReaderState::clearPendingEpubPageLoad() { asyncJobs_.clearPendingPageLoad(); }

void ReaderState::enqueuePendingPageTurn(const int direction, const char* reason) {
  asyncJobs_.enqueuePendingPageTurn(direction, reason, static_cast<int>(workerState()));
}

bool ReaderState::deferPageTurnUntilCacheStops(const int direction) {
  return asyncJobs_.deferPageTurnUntilWorkerStops(
      direction, isWorkerRunning(), static_cast<int>(workerState()), [this]() { requestWorkerCancel(); });
}

void ReaderState::armPendingEpubPageLoad(Core& core, const int targetSpine, const int targetPage,
                                         const bool requireComplete, const bool useIndexingMessage) {
  (void)core;
  const bool sameRequest = pendingEpubPageLoadActive_ && pendingEpubPageLoadTargetSpine_ == targetSpine &&
                           pendingEpubPageLoadTargetPage_ == targetPage &&
                           pendingEpubPageLoadRequireComplete_ == requireComplete &&
                           pendingEpubPageLoadUseIndexingMessage_ == useIndexingMessage;

  asyncJobs_.armPendingPageLoad(targetSpine, targetPage, requireComplete, useIndexingMessage);
  if (!sameRequest) {
    pendingEpubPageLoadStartedMs_ = millis();
    pendingEpubPageLoadLastDiagMs_ = 0;
    LOG_INF(TAG, "[ASYNC] arm page-load spine=%d page=%d complete=%u indexing=%u", targetSpine, targetPage,
            static_cast<unsigned>(requireComplete), static_cast<unsigned>(useIndexingMessage));
    pendingEpubPageLoadMessageShown_ = false;
  }
}

void ReaderState::saveAnchorMap(const ContentParser& parser, const std::string& cachePath) {
  reader::ReaderCacheController::saveAnchorMap(parser, cachePath);
}

int ReaderState::loadAnchorPage(const std::string& cachePath, const std::string& anchor) {
  return reader::ReaderCacheController::loadAnchorPage(cachePath, anchor);
}

std::vector<std::pair<std::string, uint16_t>> ReaderState::loadAnchorMap(const std::string& cachePath) {
  return reader::ReaderCacheController::loadAnchorMap(cachePath);
}

const std::vector<std::pair<std::string, uint16_t>>& ReaderState::getCachedAnchorMap(const std::string& cachePath,
                                                                                      const int spineIndex) {
  return cacheController_.getCachedAnchorMap(cachePath, spineIndex);
}

void ReaderState::invalidateAnchorMapCache() { cacheController_.invalidateAnchorMapCache(); }

void ReaderState::invalidateGlobalPageMetrics() {
  globalSectionPageMetrics_.clear();
  globalSectionPageMetrics_.shrink_to_fit();
  globalSectionPageMetricTotal_ = 0;
  globalSectionPageMetricsInitialized_ = false;
}

void ReaderState::recomputeGlobalPageMetricTotal() {
  uint32_t total = 0;
  for (const auto& metric : globalSectionPageMetrics_) {
    total += metric.pages;
  }
  globalSectionPageMetricTotal_ = total;
}

bool ReaderState::loadGlobalPageMetricsFromDisk(
    unifiedcache::UnifiedCache& cache, const std::string& bookCachePath,
    const uint32_t configHash, const int spineCount) {
  if (bookCachePath.empty() || spineCount <= 0) return false;

  // v2.0.166 — read from UnifiedCache (single `streaming.cache` per book)
  // instead of the per-book `metrics.bin` file.  Same on-payload format
  // as v2.0.153 (12-byte header + per-spine entries) just wrapped in a
  // UnifiedCache frame.  Fallback: if the old metrics.bin file still
  // exists (from a pre-v2.0.166 install), try reading it as before so
  // upgraders don't lose their cached metrics on the first open.
  std::vector<uint8_t> payload;
  const bool loadedFromUnified = cache.readSegment(
      snapix::unifiedcache::Kind::Metrics, snapix::unifiedcache::kGlobalKey, payload);
  if (!loadedFromUnified) {
    // Legacy fallback: pre-v2.0.166 metrics.bin file
    const std::string legacyPath = reader::metricsCachePath(bookCachePath);
    if (!LittleFS.exists(legacyPath.c_str())) return false;
    File f = LittleFS.open(legacyPath.c_str(), "r");
    if (!f) return false;
    payload.resize(f.size());
    const int n = f.read(payload.data(), payload.size());
    f.close();
    if (n != static_cast<int>(payload.size())) return false;
  }

  const size_t expectedBytes = kMetricsCacheHeaderBytes +
                               static_cast<size_t>(spineCount) * kMetricsCacheEntryBytes;
  if (payload.size() != expectedBytes) return false;

  const uint8_t* p = payload.data();
  const uint32_t magic = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  if (magic != kMetricsCacheMagic) return false;
  if (p[4] != kMetricsCacheVersion) return false;
  const uint16_t storedSpineCount = static_cast<uint16_t>(p[6]) | (static_cast<uint16_t>(p[7]) << 8);
  if (storedSpineCount != static_cast<uint16_t>(spineCount)) return false;
  const uint32_t storedHash = static_cast<uint32_t>(p[8]) | (static_cast<uint32_t>(p[9]) << 8) |
                              (static_cast<uint32_t>(p[10]) << 16) | (static_cast<uint32_t>(p[11]) << 24);
  if (storedHash != configHash) return false;

  globalSectionPageMetrics_.assign(static_cast<size_t>(spineCount), SectionPageMetric{});
  for (int i = 0; i < spineCount; ++i) {
    const uint8_t* e = p + kMetricsCacheHeaderBytes + i * kMetricsCacheEntryBytes;
    auto& m = globalSectionPageMetrics_[static_cast<size_t>(i)];
    m.pages = static_cast<uint16_t>(e[0]) | (static_cast<uint16_t>(e[1]) << 8);
    m.byteSize = static_cast<uint32_t>(e[2]) | (static_cast<uint32_t>(e[3]) << 8) |
                 (static_cast<uint32_t>(e[4]) << 16) | (static_cast<uint32_t>(e[5]) << 24);
    m.exact = e[6] != 0;
  }
  return true;
}

void ReaderState::saveGlobalPageMetricsToDisk(
    unifiedcache::UnifiedCache& cache, const uint32_t configHash) const {
  if (globalSectionPageMetrics_.empty()) return;

  const uint16_t spineCount = static_cast<uint16_t>(
      std::min<size_t>(globalSectionPageMetrics_.size(), static_cast<size_t>(UINT16_MAX)));
  const size_t total = kMetricsCacheHeaderBytes + spineCount * kMetricsCacheEntryBytes;

  // v2.0.166 — write to UnifiedCache.  Build the payload in a local buffer
  // (~400 bytes for 55 sections, trivial) and hand it to UnifiedCache::
  // writeSegment which appends a Metrics frame atomically.
  std::vector<uint8_t> payload(total);
  uint8_t* p = payload.data();
  p[0] = static_cast<uint8_t>(kMetricsCacheMagic);
  p[1] = static_cast<uint8_t>(kMetricsCacheMagic >> 8);
  p[2] = static_cast<uint8_t>(kMetricsCacheMagic >> 16);
  p[3] = static_cast<uint8_t>(kMetricsCacheMagic >> 24);
  p[4] = kMetricsCacheVersion;
  p[5] = 0;
  p[6] = static_cast<uint8_t>(spineCount);
  p[7] = static_cast<uint8_t>(spineCount >> 8);
  p[8] = static_cast<uint8_t>(configHash);
  p[9] = static_cast<uint8_t>(configHash >> 8);
  p[10] = static_cast<uint8_t>(configHash >> 16);
  p[11] = static_cast<uint8_t>(configHash >> 24);
  for (uint16_t i = 0; i < spineCount; ++i) {
    const auto& m = globalSectionPageMetrics_[i];
    uint8_t* e = p + kMetricsCacheHeaderBytes + i * kMetricsCacheEntryBytes;
    e[0] = static_cast<uint8_t>(m.pages);
    e[1] = static_cast<uint8_t>(m.pages >> 8);
    e[2] = static_cast<uint8_t>(m.byteSize);
    e[3] = static_cast<uint8_t>(m.byteSize >> 8);
    e[4] = static_cast<uint8_t>(m.byteSize >> 16);
    e[5] = static_cast<uint8_t>(m.byteSize >> 24);
    e[6] = static_cast<uint8_t>(m.exact ? 1 : 0);
  }

  cache.writeSegment(snapix::unifiedcache::Kind::Metrics, snapix::unifiedcache::kGlobalKey,
                     payload.data(), payload.size());
}

void ReaderState::initializeGlobalPageMetrics(Core& core, const int currentSectionTotalPages,
                                              const bool currentSectionIsPartial) {
  const uint32_t metricsStartedMs = reader::perfMsNow();
  invalidateGlobalPageMetrics();

  const ContentType type = core.content.metadata().type;

  const Theme& theme = THEME_MANAGER.current();
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  const auto config = core.settings.getRenderConfig(theme, vp.width, vp.height);

  int spineCount = 0;

  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return;
    spineCount = provider->getEpub()->getSpineItemsCount();
    if (spineCount <= 0) return;
  } else if (type == ContentType::Fb2 && fb2UsesSectionNavigation(core.content.asFb2())) {
    auto* provider = core.content.asFb2();
    auto* fb2 = provider ? provider->getFb2() : nullptr;
    if (!fb2 || fb2->tocCount() == 0) return;

    spineCount = static_cast<int>(fb2->tocCount());
  } else {
    return;
  }

  // Hit the tiny persisted metrics frame before doing any per-section work.
  // The source fingerprint is part of the parent cache path/lifecycle, so a
  // valid config+spine-count match is already safe to reuse as-is.
  const std::string bookCachePath = bookCachePathForMetrics(core);
  const uint32_t configHash = computeMetricsConfigHash(config);
  auto metricsCache =
      snapix::unifiedcache::UnifiedCache::shared(bookCachePath);
  if (loadGlobalPageMetricsFromDisk(metricsCache, bookCachePath, configHash,
                                    spineCount)) {
    bool exactMeasurementChanged = false;
    if (currentSpineIndex_ >= 0 && currentSpineIndex_ < spineCount &&
        currentSectionTotalPages > 0) {
      auto& metric =
          globalSectionPageMetrics_[static_cast<size_t>(currentSpineIndex_)];
      const auto currentPages =
          static_cast<uint16_t>(std::max(1, currentSectionTotalPages));
      if (!currentSectionIsPartial) {
        if (!metric.exact || metric.pages != currentPages) {
          // A complete live cache is authoritative.  In particular, do not
          // retain a larger old estimate while marking it exact.
          metric.pages = currentPages;
          metric.exact = true;
          exactMeasurementChanged = true;
        }
      } else if (!metric.exact && currentPages > metric.pages) {
        metric.pages = currentPages;
      }
    }
    globalSectionPageMetricsInitialized_ = true;
    if (exactMeasurementChanged) {
      recalibrateGlobalPageEstimates();
      saveGlobalPageMetricsToDisk(metricsCache, configHash);
    } else {
      recomputeGlobalPageMetricTotal();
    }
    readerPerfLog("global-metrics-init", metricsStartedMs,
                  "(source=cache sections=%d)", spineCount);
    return;
  }

  // Cache miss: derive only lightweight byte-size estimates in one I/O pass.
  // Do not probe every page-cache file here: this function runs inside the
  // first visible page render, and the old O(sectionCount) probe loop added
  // 7-19 seconds of synchronous latency on hardware.
  std::vector<size_t> itemSizes(static_cast<size_t>(spineCount), 0);
  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider->getEpub()->getSpineItemSizes(itemSizes)) {
      LOG_INF(TAG, "[METRICS] EPUB batch size lookup incomplete; using fallback estimates");
    }
  } else {
    auto* provider = core.content.asFb2();
    auto* fb2 = provider ? provider->getFb2() : nullptr;
    std::vector<uint32_t> sourceOffsets;
    if (!fb2 || !fb2->getTocSourceOffsets(sourceOffsets) ||
        sourceOffsets.size() != static_cast<size_t>(spineCount)) {
      LOG_INF(TAG, "[METRICS] FB2 bulk TOC lookup failed; using fallback estimates");
      sourceOffsets.assign(static_cast<size_t>(spineCount), 0);
    }

    // Derive per-section byte sizes from consecutive sourceOffset deltas.
    // For the last section we cannot use totalFileSize because FB2 files
    // typically have large <binary> blocks (base64 cover images, ~60% of
    // file size) after the closing </body>.  Using totalFileSize as the
    // end boundary makes the last section appear 10-30x larger than it
    // really is.  Instead, cap it using the median of preceding sections.
    for (int i = 0; i < spineCount; ++i) {
      if (i + 1 < spineCount) {
        const uint32_t current = sourceOffsets[static_cast<size_t>(i)];
        const uint32_t next = sourceOffsets[static_cast<size_t>(i + 1)];
        itemSizes[static_cast<size_t>(i)] =
            next > current ? next - current : 0;
      }
      // Last section: leave at 0 for now, estimate below.
    }
    if (spineCount > 1) {
      // Estimate the last section from the median of non-zero deltas.
      std::vector<size_t> nonZeroSizes;
      nonZeroSizes.reserve(static_cast<size_t>(spineCount));
      for (int i = 0; i < spineCount - 1; ++i) {
        if (itemSizes[static_cast<size_t>(i)] > 0) {
          nonZeroSizes.push_back(itemSizes[static_cast<size_t>(i)]);
        }
      }
      if (!nonZeroSizes.empty()) {
        std::sort(nonZeroSizes.begin(), nonZeroSizes.end());
        const size_t median = nonZeroSizes[nonZeroSizes.size() / 2];
        itemSizes[static_cast<size_t>(spineCount - 1)] = median;
      }
    } else if (spineCount == 1) {
      // Single-section book: use the first section offset to content start.
      const uint32_t sourceOffset = sourceOffsets[0];
      const size_t contentEstimate = fb2->getFileSize() > sourceOffset
          ? (fb2->getFileSize() - sourceOffset) / 3  // crude: ~1/3 of FB2 is text
          : 0;
      itemSizes[0] = contentEstimate;
    }
  }

  globalSectionPageMetrics_.resize(static_cast<size_t>(spineCount));

  for (int spineIndex = 0; spineIndex < spineCount; ++spineIndex) {
    const size_t itemSize = itemSizes[static_cast<size_t>(spineIndex)];
    auto& metric = globalSectionPageMetrics_[static_cast<size_t>(spineIndex)];
    metric.byteSize = static_cast<uint32_t>(itemSize);
    if (spineIndex == currentSpineIndex_ && currentSectionTotalPages > 0) {
      metric.pages =
          static_cast<uint16_t>(std::max(1, currentSectionTotalPages));
      metric.exact = !currentSectionIsPartial;
    }
  }

  size_t calibrationBytes = 0;
  uint32_t calibrationPages = 0;
  for (const auto& metric : globalSectionPageMetrics_) {
    if (metric.exact && metric.byteSize > 0 && metric.pages > 0) {
      calibrationBytes += metric.byteSize;
      calibrationPages += metric.pages;
    }
  }
  const size_t bytesPerPage =
      calibrationPages > 0 ? std::max<size_t>(256, calibrationBytes / calibrationPages) : kEstimatedBytesPerPage;

  for (int spineIndex = 0; spineIndex < spineCount; ++spineIndex) {
    auto& metric = globalSectionPageMetrics_[static_cast<size_t>(spineIndex)];
    const size_t itemSize = itemSizes[static_cast<size_t>(spineIndex)];
    const uint16_t estimatedPages = estimatePagesForBytes(itemSize, bytesPerPage);
    if (metric.pages == 0) {
      metric.pages = estimatedPages;
    } else if (!metric.exact && estimatedPages > metric.pages) {
      metric.pages = estimatedPages;
    }
  }

  recomputeGlobalPageMetricTotal();
  globalSectionPageMetricsInitialized_ = true;

  // Persist the freshly derived metrics so the next book-open session can take
  // the fast path above.  Save once at init time; further refinements as the
  // user reads (becameExact in updateGlobalPageMetrics) trigger their own
  // saves.  Best-effort — failure leaves the next session repeating the batch
  // metadata pass, but never breaks correctness.
  saveGlobalPageMetricsToDisk(metricsCache, configHash);
  readerPerfLog("global-metrics-init", metricsStartedMs,
                "(source=batch sections=%d)", spineCount);
}

void ReaderState::updateGlobalPageMetrics(Core& core, const int currentSectionTotalPages, const bool currentSectionIsPartial) {
  const ContentType type = core.content.metadata().type;
  if (type != ContentType::Epub && !(type == ContentType::Fb2 && fb2UsesSectionNavigation(core.content.asFb2()))) {
    return;
  }

  if (!globalSectionPageMetricsInitialized_) {
    initializeGlobalPageMetrics(core, currentSectionTotalPages, currentSectionIsPartial);
  }

  if (!globalSectionPageMetricsInitialized_ || currentSpineIndex_ < 0 ||
      currentSpineIndex_ >= static_cast<int>(globalSectionPageMetrics_.size()) || currentSectionTotalPages <= 0) {
    return;
  }

  auto& metric = globalSectionPageMetrics_[static_cast<size_t>(currentSpineIndex_)];
  const auto currentPages = static_cast<uint16_t>(std::max(1, currentSectionTotalPages));
  bool becameExact = false;
  bool changed = false;

  if (!currentSectionIsPartial) {
    if (!metric.exact || metric.pages != currentPages) {
      metric.pages = currentPages;
      if (!metric.exact) becameExact = true;
      metric.exact = true;
      changed = true;
    }
  } else if (!metric.exact && currentPages > metric.pages) {
    metric.pages = currentPages;
    changed = true;
  }

  if (becameExact) {
    // A new section just finished — its (byteSize, pages) pair tightens the
    // bytes-per-page calibration.  Re-estimate the still-partial sections so
    // the global total reflects the improved sample size and recompute total.
    recalibrateGlobalPageEstimates();

    // v2.0.153 — persist the tightened metrics so the next session loads them
    // verbatim instead of re-probing.  Only on becameExact (not every partial
    // tick) to keep LittleFS write traffic bounded to spineCount writes per
    // book lifetime.  Best-effort; failures are silent.
    const std::string bookCachePath = bookCachePathForMetrics(core);
    if (!bookCachePath.empty()) {
      const Theme& theme = THEME_MANAGER.current();
      const auto vp = getReaderViewport(core.settings.statusBar != 0);
      const auto config = core.settings.getRenderConfig(theme, vp.width, vp.height);
      auto cache =
          snapix::unifiedcache::UnifiedCache::shared(bookCachePath);
      saveGlobalPageMetricsToDisk(cache, computeMetricsConfigHash(config));
    }
  } else if (changed) {
    recomputeGlobalPageMetricTotal();
  }
}

void ReaderState::recalibrateGlobalPageEstimates() {
  if (!globalSectionPageMetricsInitialized_) return;

  size_t calibBytes = 0;
  uint32_t calibPages = 0;
  for (const auto& m : globalSectionPageMetrics_) {
    if (m.exact && m.byteSize > 0 && m.pages > 0) {
      calibBytes += m.byteSize;
      calibPages += m.pages;
    }
  }
  if (calibPages == 0) {
    // No exact sections yet — leave existing estimates, just recompute total.
    recomputeGlobalPageMetricTotal();
    return;
  }

  const size_t bytesPerPage = std::max<size_t>(256, calibBytes / calibPages);

  // Re-estimate non-exact sections using the freshly calibrated ratio.  This
  // is what fixes "approximate total never converges to reality" — every time
  // a section becomes exact, the remaining estimates rebase on the latest
  // bytes-per-page from the user's actual font / spacing settings.
  for (auto& m : globalSectionPageMetrics_) {
    if (m.exact || m.byteSize == 0) continue;
    const uint16_t newEstimate = estimatePagesForBytes(m.byteSize, bytesPerPage);
    if (newEstimate != m.pages && newEstimate > 0) {
      m.pages = newEstimate;
    }
  }

  recomputeGlobalPageMetricTotal();
}

ReaderState::GlobalPageMetrics ReaderState::resolveGlobalPageMetrics(Core& core, const int currentSectionTotalPages,
                                                                     const bool currentSectionIsPartial) {
  GlobalPageMetrics metrics;
  const ContentType type = core.content.metadata().type;

  if (type == ContentType::Xtc) {
    metrics.currentPage = static_cast<int>(currentPage_) + 1;
    metrics.totalPages = static_cast<int>(std::max<uint32_t>(core.content.pageCount(), metrics.currentPage));
    return metrics;
  }

  if (type == ContentType::Epub) {
    updateGlobalPageMetrics(core, currentSectionTotalPages, currentSectionIsPartial);
    if (globalSectionPageMetricsInitialized_ && !globalSectionPageMetrics_.empty()) {
      const int clampedSpine = std::clamp(currentSpineIndex_, 0, static_cast<int>(globalSectionPageMetrics_.size()) - 1);
      uint32_t pagesBefore = 0;
      bool totalIsExact = true;
      for (int i = 0; i < clampedSpine; ++i) {
        const auto& sectionMetric = globalSectionPageMetrics_[static_cast<size_t>(i)];
        pagesBefore += sectionMetric.pages;
        totalIsExact = totalIsExact && sectionMetric.exact;
      }
      for (int i = clampedSpine; i < static_cast<int>(globalSectionPageMetrics_.size()); ++i) {
        totalIsExact = totalIsExact && globalSectionPageMetrics_[static_cast<size_t>(i)].exact;
      }
      metrics.currentPage = static_cast<int>(pagesBefore) + std::max(currentSectionPage_, 0) + 1;
      metrics.totalPages = static_cast<int>(std::max<uint32_t>(globalSectionPageMetricTotal_, metrics.currentPage));
      metrics.totalIsExact = totalIsExact;
      return metrics;
    }
  }

  if (type == ContentType::Fb2 && fb2UsesSectionNavigation(core.content.asFb2())) {
    updateGlobalPageMetrics(core, currentSectionTotalPages, currentSectionIsPartial);
    if (globalSectionPageMetricsInitialized_ && !globalSectionPageMetrics_.empty()) {
      const int clampedSpine = std::clamp(currentSpineIndex_, 0, static_cast<int>(globalSectionPageMetrics_.size()) - 1);
      uint32_t pagesBefore = 0;
      bool totalIsExact = true;
      for (int i = 0; i < clampedSpine; ++i) {
        const auto& sectionMetric = globalSectionPageMetrics_[static_cast<size_t>(i)];
        pagesBefore += sectionMetric.pages;
        totalIsExact = totalIsExact && sectionMetric.exact;
      }
      for (int i = clampedSpine; i < static_cast<int>(globalSectionPageMetrics_.size()); ++i) {
        totalIsExact = totalIsExact && globalSectionPageMetrics_[static_cast<size_t>(i)].exact;
      }
      metrics.currentPage = static_cast<int>(pagesBefore) + std::max(currentSectionPage_, 0) + 1;
      metrics.totalPages = static_cast<int>(std::max<uint32_t>(globalSectionPageMetricTotal_, metrics.currentPage));
      metrics.totalIsExact = totalIsExact;
      return metrics;
    }

    // Fallback: per-section metrics not available yet — use crude scaling.
    auto* provider = core.content.asFb2();
    auto* fb2 = provider ? provider->getFb2() : nullptr;
    const int estimatedTotalPages =
        std::max({static_cast<int>(core.content.pageCount()), std::max(currentSectionTotalPages, 0), 1});

    int resolvedPage = std::max(currentSectionPage_, 0) + 1;
    if (fb2 && currentSpineIndex_ >= 0 && currentSpineIndex_ < static_cast<int>(fb2->tocCount()) && fb2->getFileSize() > 0) {
      const Fb2::TocItem item = fb2->getTocItem(static_cast<uint16_t>(currentSpineIndex_));
      const uint64_t scaled = static_cast<uint64_t>(estimatedTotalPages) * item.sourceOffset;
      resolvedPage = static_cast<int>(scaled / fb2->getFileSize()) + std::max(currentSectionPage_, 0) + 1;
    }

    metrics.currentPage = std::max(1, resolvedPage);
    metrics.totalPages = std::max(estimatedTotalPages, metrics.currentPage);
    metrics.totalIsExact = false;
    return metrics;
  }

  metrics.currentPage = std::max(currentSectionPage_, 0) + 1;
  metrics.totalPages = static_cast<int>(std::max<uint32_t>(core.content.pageCount(), metrics.currentPage));
  metrics.totalIsExact = !currentSectionIsPartial;
  return metrics;
}

int ReaderState::calcFirstContentSpine(bool hasCover, int textStartIndex, size_t spineCount) {
  return reader::ReaderCacheController::calcFirstContentSpine(hasCover, textStartIndex, spineCount);
}

void ReaderState::createOrExtendCacheImpl(ContentParser& parser, const std::string& cachePath,
                                          const RenderConfig& config, uint16_t batchSize) {
  cacheController_.createOrExtendCacheImpl(parser, cachePath, config, batchSize);
}

ReaderState::BackgroundCachePlan ReaderState::planBackgroundCacheWork(Core& core) { return cacheController_.planBackgroundCacheWork(core); }

bool ReaderState::shouldContinueIdleBackgroundCaching(Core& core) {
  return cacheController_.shouldContinueIdleBackgroundCaching(core);
}

bool ReaderState::prefetchNextEpubSpineCache(Core& core, const RenderConfig& config, const int activeSpineIndex,
                                             const bool coverExists, const int textStartIndex,
                                             const bool allowFarSweep, const std::function<bool()>& shouldAbort) {
  return cacheController_.prefetchNextEpubSpineCache(core, config, activeSpineIndex, coverExists, textStartIndex,
                                                     allowFarSweep, shouldAbort);
}

ReaderState::ReaderState(GfxRenderer& renderer)
    : renderer_(renderer),
      xtcRenderer_(renderer),
      currentPage_(0),
      needsRender_(true),
      contentLoaded_(false),
      currentSpineIndex_(0),
      currentSectionPage_(0),
      cacheController_(renderer_,
                       reader::PositionRefs{currentPage_, currentSpineIndex_, currentSectionPage_, lastRenderedSpineIndex_,
                                            lastRenderedSectionPage_, hasCover_, textStartIndex_}),
      pageCache_(cacheController_.resourceState().pageCache),
      parser_(cacheController_.resourceState().parser),
      parserSpineIndex_(cacheController_.resourceState().parserSpineIndex),
      lookaheadParser_(cacheController_.resourceState().lookaheadParser),
      lookaheadParserSpineIndex_(cacheController_.resourceState().lookaheadParserSpineIndex),
      pagesUntilFullRefresh_(1),
      thumbnailDone_(cacheController_.thumbnailDoneRef()),
      lastIdleBackgroundKickMs_(cacheController_.lastIdleBackgroundKickMsRef()),
      lastReaderInteractionMs_(cacheController_.lastReaderInteractionMsRef()),
      holdNavigated_(navigationController_.holdNavigatedRef()),
      powerPressStartedMs_(navigationController_.powerPressStartedMsRef()),
      warmedNextPage_(cacheController_.warmedNextPageRef()),
      warmedNextNextPage_(cacheController_.warmedNextNextPageRef()),
      renderOverridePage_(cacheController_.renderOverridePageRef()),
      pendingTocJumpActive_(asyncJobs_.pendingTocJumpActiveRef()),
      pendingTocJumpIndexingShown_(asyncJobs_.pendingTocJumpIndexingShownRef()),
      pendingTocJumpDeferredDisplay_(asyncJobs_.pendingTocJumpDeferredDisplayRef()),
      pendingTocJumpTargetSpine_(asyncJobs_.pendingTocJumpTargetSpineRef()),
      pendingTocJumpTargetPageHint_(asyncJobs_.pendingTocJumpTargetPageHintRef()),
      pendingTocJumpAnchor_(asyncJobs_.pendingTocJumpAnchorRef()),
      pendingTocJumpRetryCount_(asyncJobs_.pendingTocJumpRetryCountRef()),
      pendingTocJumpStartedMs_(asyncJobs_.pendingTocJumpStartedMsRef()),
      pendingTocJumpLastDiagMs_(asyncJobs_.pendingTocJumpLastDiagMsRef()),
      pendingTocFirstPageReady_(asyncJobs_.pendingTocFirstPageReadyRef()),
      pendingEpubPageLoadActive_(asyncJobs_.pendingPageLoadActiveRef()),
      pendingEpubPageLoadMessageShown_(asyncJobs_.pendingPageLoadMessageShownRef()),
      pendingEpubPageLoadRequireComplete_(asyncJobs_.pendingPageLoadRequireCompleteRef()),
      pendingEpubPageLoadUseIndexingMessage_(asyncJobs_.pendingPageLoadUseIndexingMessageRef()),
      pendingEpubPageLoadTargetSpine_(asyncJobs_.pendingPageLoadTargetSpineRef()),
      pendingEpubPageLoadTargetPage_(asyncJobs_.pendingPageLoadTargetPageRef()),
      pendingEpubPageLoadRetryCount_(asyncJobs_.pendingPageLoadRetryCountRef()),
      pendingEpubPageLoadStartedMs_(asyncJobs_.pendingPageLoadStartedMsRef()),
      pendingEpubPageLoadLastDiagMs_(asyncJobs_.pendingPageLoadLastDiagMsRef()),
      pendingEpubPageLoadNextRetryMs_(asyncJobs_.pendingPageLoadNextRetryMsRef()),
      queuedPendingEpubTurn_(asyncJobs_.queuedPendingPageTurnRef()),
      queuedPendingEpubTurnQueuedMs_(asyncJobs_.queuedPendingPageTurnQueuedMsRef()),
      lastCachePreemptRequestedMs_(asyncJobs_.lastCachePreemptRequestedMsRef()),
      tocView_{} {
  asyncJobs_.setBackgroundCacheHandler(
      [this](const reader::ReaderAsyncJobsController::BackgroundCacheRequest& request,
             const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort) {
        runBackgroundCacheJob(request, shouldAbort);
      });
  asyncJobs_.setTocJumpHandler([this](const reader::ReaderAsyncJobsController::TocJumpRequest& request,
                                      const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort) {
    runTocJumpJob(request, shouldAbort);
  });
  asyncJobs_.setPageFillHandler([this](const reader::ReaderAsyncJobsController::PageFillRequest& request,
                                       const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort) {
    runPageFillJob(request, shouldAbort);
  });
  contentPath_[0] = '\0';
}

ReaderState::~ReaderState() {
  stopBackgroundCaching();
  asyncJobs_.stopWorker();
}

void ReaderState::setContentPath(const char* path) {
  if (path) {
    strncpy(contentPath_, path, sizeof(contentPath_) - 1);
    contentPath_[sizeof(contentPath_) - 1] = '\0';
  } else {
    contentPath_[0] = '\0';
  }
  cacheController_.setContentPath(contentPath_);
}

void ReaderState::enter(Core& core) {
  // Free memory from the previous reader session before loading a new book.
  // On ESP32 the heap is non-compacting, so unloading long-lived font caches is
  // one of the few effective ways to recover a larger contiguous block.
  //
  // Skip the font unload (and width-cache invalidation) when the new book uses
  // the SAME font as the previous one — keeps the warm bitmap LRU cache in
  // place so the first page renders instantly instead of paying the 15-30s
  // cold-load penalty for Cyrillic/CJK glyphs.
  const reader::HeapState heapBeforeTrim = reader::readHeapState();
  const size_t fontBytesBeforeTrim = FONT_MANAGER.getTotalFontMemoryUsage();
  THEME_MANAGER.clearCache();
  const Theme& currentTheme = THEME_MANAGER.current();
  const char* newReaderFontFamily = core.settings.getReaderFontFamily(currentTheme);
  const bool sameReaderFontAsBefore = FONT_MANAGER.isReaderFontAlreadyActive(newReaderFontFamily);
  if (!sameReaderFontAsBefore) {
    FONT_MANAGER.unloadReaderFonts();
    renderer_.clearWidthCache();
  } else {
    // Same font is preserved across the book switch — the streaming font
    // bitmap cache holds glyphs from the previous book.  Across many book
    // switches these scattered allocations fragment the heap (we've seen
    // largest=7668 after 3 books) and block background cache extension.
    //
    // Drop the bitmap cache (cheap: only re-warmed on first page render of
    // the new book, ~200ms) but KEEP the font's intervals + glyph table +
    // open SD file.  This defragments the heap without paying the cold-load
    // penalty of a full unload+reload.
    FONT_MANAGER.clearStreamingBitmapCaches();
    renderer_.clearWidthCache();
  }
  const reader::HeapState heapAfterTrim = reader::readHeapState();
  LOG_INF(TAG, "Entry heap trim: free=%u->%u largest=%u->%u fontBytes=%u sameFont=%u",
          static_cast<unsigned>(heapBeforeTrim.freeBytes), static_cast<unsigned>(heapAfterTrim.freeBytes),
          static_cast<unsigned>(heapBeforeTrim.largestBlock), static_cast<unsigned>(heapAfterTrim.largestBlock),
          static_cast<unsigned>(fontBytesBeforeTrim), static_cast<unsigned>(sameReaderFontAsBefore));

  contentLoaded_ = false;
  loadFailed_ = false;
  needsRender_ = true;
  navigationController_.resetSession();
  asyncJobs_.clearPendingTocJump();
  asyncJobs_.clearPendingPageLoad();
  asyncJobs_.clearQueuedPageTurns();
  asyncJobs_.clearPageLoadBlock();
  asyncJobs_.pendingRefresh().clear();
  stopBackgroundCaching();  // Ensure any previous task is stopped
  cacheController_.resetSession();
  invalidateGlobalPageMetrics();
  currentSpineIndex_ = 0;
  currentSectionPage_ = 0;  // Will be set to -1 after progress load if at start
  pagesUntilFullRefresh_ = (core.settings.getPagesPerRefreshValue() == 0) ? 0 : 1;
  directUiTransition_ = core.pendingDirectReaderTransition;
  core.pendingDirectReaderTransition = false;
  resumeBackgroundCachingAfterRender_ = false;
  lastReaderInteractionMs_ = millis();

  // Always prefer an explicitly queued path from UI transitions.
  if (core.buf.path[0] != '\0') {
    strncpy(contentPath_, core.buf.path, sizeof(contentPath_) - 1);
    contentPath_[sizeof(contentPath_) - 1] = '\0';
    core.buf.path[0] = '\0';
    cacheController_.setContentPath(contentPath_);
  }

  // Determine source state from boot transition
  const auto& transition = getTransition();
  if (directUiTransition_) {
    sourceState_ = core.pendingReaderReturnState;
  } else {
    sourceState_ =
        (transition.isValid() && transition.returnTo == ReturnTo::FILE_MANAGER) ? StateId::FileList : StateId::Home;
  }

  LOG_INF(TAG, "Entering with path: %s", contentPath_);

  if (contentPath_[0] == '\0') {
    LOG_ERR(TAG, "No content path set");
    return;
  }

  // Apply orientation setting to renderer
  switch (core.settings.orientation) {
    case Settings::Portrait:
      renderer_.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case Settings::LandscapeCW:
      renderer_.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case Settings::Inverted:
      renderer_.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case Settings::LandscapeCCW:
      renderer_.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      renderer_.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
  }

  // Show a "Loading..." banner overlay immediately so the user gets visual
  // feedback while the (potentially multi-second) content parsing runs.
  // Uses the builtin UI font because reader streaming fonts were just unloaded.
  // Skip when waking from SleepPage — the "Sleeping" banner is already on the
  // e-ink panel over the page, so the user already has visual context.  The first
  // page render will clear the banner with drive-all.
  if (core.wokeFromSleep && core.settings.sleepScreen == Settings::SleepPage) {
    forceCleanRefreshOnNext_ = true;
  } else {
    renderCenteredStatusMessage(core, "Loading...", THEME_MANAGER.current().uiFontId);
  }

  // Open content using ContentHandle
  auto result = core.content.open(contentPath_, SNAPIX_CACHE_DIR);
  if (!result.ok()) {
    LOG_ERR(TAG, "Failed to open content: %s", errorToString(result.err));
    // Store error message for ErrorState to display
    snprintf(core.buf.text, sizeof(core.buf.text), "Cannot open file:\n%s", errorToString(result.err));
    loadFailed_ = true;  // Mark as failed for update() to transition to error state
    return;
  }

  contentLoaded_ = true;

  // FB2 files without titled sections are parsed as one full stream.  Their
  // first marker/index build can legitimately take minutes, so replace the
  // generic banner with an honest static warning as soon as metadata is known.
  if (core.content.metadata().type == ContentType::Fb2 && core.content.tocCount() == 0) {
    renderLoadingStatusMessage(core);
  }

  // Save last book path to settings
  strncpy(core.settings.lastBookPath, contentPath_, sizeof(core.settings.lastBookPath) - 1);
  core.settings.lastBookPath[sizeof(core.settings.lastBookPath) - 1] = '\0';
  core.settings.save(core.storage);

  // Setup cache directories for all content types
  // Reset state for new book
  textStartIndex_ = 0;
  hasCover_ = false;
  thumbnailDone_ = false;
  switch (core.content.metadata().type) {
    case ContentType::Epub: {
      auto* provider = core.content.asEpub();
      if (provider && provider->getEpub()) {
        const auto* epub = provider->getEpub();
        epub->setupCacheDir();
        // Get the spine index for the first text content (from <guide> element)
        textStartIndex_ = epub->getSpineIndexForTextReference();
        LOG_DBG(TAG, "Text starts at spine index %d", textStartIndex_);
      }
      break;
    }
    case ContentType::Txt: {
      auto* provider = core.content.asTxt();
      if (provider && provider->getTxt()) {
        provider->getTxt()->setupCacheDir();
      }
      // v2.0.194 — TXT has no images, no BMPs to render.  Defensively
      // free any pinned bitmap row buffers (~2.6 KB) + drop any residual
      // image cache entries left over from the previous book (image
      // BMPs can be 5-30 KB each).  These are no-ops if already empty;
      // ensureBitmapRowBuffers() lazily re-allocs if a BMP is ever
      // needed during the TXT session (e.g. cover BMP).
      renderer_.freeBitmapRowBuffers();
      renderer_.imageCache().clear();
      break;
    }
    case ContentType::Markdown: {
      auto* provider = core.content.asMarkdown();
      if (provider && provider->getMarkdown()) {
        provider->getMarkdown()->setupCacheDir();
      }
      // v2.0.194 — same defensive cleanup as TXT.  Markdown supports
      // a few embedded image syntaxes but the typical case is text-
      // only; same lazy re-alloc applies if a BMP becomes needed.
      renderer_.freeBitmapRowBuffers();
      renderer_.imageCache().clear();
      break;
    }
    case ContentType::Fb2: {
      auto* provider = core.content.asFb2();
      if (provider && provider->getFb2()) {
        provider->getFb2()->setupCacheDir();
      }
      break;
    }
    case ContentType::Html: {
      auto* provider = core.content.asHtml();
      if (provider && provider->getHtml()) {
        provider->getHtml()->setupCacheDir();
      }
      break;
    }
    default:
      break;
  }

  // Load saved progress
  ContentType type = core.content.metadata().type;
  auto progress = ProgressManager::load(core, core.content.cacheDir(), type);
  progress = ProgressManager::validate(core, type, progress);
  currentSpineIndex_ = progress.spineIndex;
  currentSectionPage_ = progress.sectionPage;
  currentPage_ = progress.flatPage;

  bookmarkCount_ = BookmarkManager::load(core, core.content.cacheDir(), bookmarks_, BookmarkManager::MAX_BOOKMARKS);

  // If at start of book and showImages enabled, begin at cover
  // Skip for XTC — uses flat page indexing, no cover page concept in reader
  if (type != ContentType::Xtc && currentSpineIndex_ == 0 && currentSectionPage_ == 0 && core.settings.showImages) {
    currentSectionPage_ = -1;  // Cover page
  }

  // Initialize last rendered to loaded position (until first render)
  lastRenderedSpineIndex_ = currentSpineIndex_;
  lastRenderedSectionPage_ = currentSectionPage_;
  lastIdleBackgroundKickMs_ = 0;

  LOG_INF(TAG, "Loaded: %s", core.content.metadata().title);
  activeCore_ = &core;
  if (!asyncJobs_.startWorker()) {
    LOG_ERR(TAG, "[ASYNC] failed to start long-lived reader worker");
  }

  // Eagerly load existing page cache from disk so the first render can display
  // the page immediately (~20ms SD read) instead of deferring to the background
  // worker (~seconds for cold extend + prefetch).  The "Loading…" banner's
  // display refresh is synchronous (displayBufferDriveAll), so the SPI bus is
  // free at this point and there is no contention with the panel.
  loadCacheFromDisk(core);

  // Delay background caching until after the first reader frame is shown.
  // Display and SD share the same SPI bus on X4, so starting PageCache here
  // can race with the initial screen refresh and trip SPI transaction asserts.
}

void ReaderState::exit(Core& core) {
  LOG_INF(TAG, "Exiting");
  invalidateGlobalPageMetrics();

  // Stop background caching task first - BackgroundTask::stop() waits properly
  stopBackgroundCaching();
  asyncJobs_.stopWorker();
  asyncJobs_.clearPendingTocJump();
  asyncJobs_.clearPendingPageLoad();
  asyncJobs_.pendingRefresh().clear();

#if defined(SNAPIX_MARKERIZED_RENDER) && SNAPIX_MARKERIZED_RENDER
  // v2.0.128 R4.a — persist the current spine's page-offset cache to
  // its `.idx` sidecar so the NEXT book-open session can skip the
  // MEASURE-only walk.  Same atomic-write pattern as the in-render
  // save path (write `.work` first, then rename).
  if (streamOffsetCacheSpine_ >= 0 && !streamOffsetCache_.empty() &&
      !streamOffsetCacheBookPath_.empty() && streamOffsetCacheKey_ >= 0) {
    const size_t needed = snapix::smolport::kPageIndexHeaderBytes +
                          streamOffsetCache_.size() * snapix::smolport::kPageIndexEntryBytes;
    if (needed <= 4096) {
      uint8_t serdebuf[4096];
      const size_t wrote = snapix::smolport::serializePageIndex(
          streamOffsetCache_.data(), streamOffsetCache_.size(), streamOffsetCacheConfigHash_,
          serdebuf, sizeof(serdebuf));
      if (wrote > 0) {
        // v2.0.167 — UnifiedCache::Idx segment write replaces the legacy
        // .work + rename atomic-publish.
        auto ucache = snapix::unifiedcache::UnifiedCache::shared(streamOffsetCacheBookPath_);
        if (ucache.writeSegment(snapix::unifiedcache::Kind::Idx,
                                  static_cast<uint16_t>(streamOffsetCacheKey_), serdebuf, wrote)) {
          LOG_INF(TAG, "[STREAM] idx saved (exit) spine=%d entries=%u (UnifiedCache::Idx)",
                  streamOffsetCacheSpine_,
                  static_cast<unsigned>(streamOffsetCache_.size()));
        }
      }
    }
    streamOffsetCache_.clear();
    streamOffsetCacheSpine_ = -1;
    streamOffsetCacheBookPath_.clear();
    streamOffsetCacheKey_ = -1;
  }
#endif

  if (contentLoaded_) {
    // Save progress at last rendered position (not current requested position)
    ProgressManager::Progress progress;
    // If on cover, save as (0, 0) - cover is implicit start
    progress.spineIndex = (lastRenderedSectionPage_ == -1) ? 0 : lastRenderedSpineIndex_;
    progress.sectionPage = (lastRenderedSectionPage_ == -1) ? 0 : lastRenderedSectionPage_;
    progress.flatPage = currentPage_;
    ProgressManager::save(core, core.content.cacheDir(), core.content.metadata().type, progress);
    saveBookmarks(core);

    // Safe to reset - task is stopped, we own pageCache_/parser_
    cacheController_.resetSession();
    core.content.close();
  }

  // v2.0.180 — PROACTIVE JPEG decoder release on book exit.
  //
  // The shared JPEGDEC instance (sizeof ~25 KB on ESP32-C3) is lazily
  // allocated on first cover/image decode and kept heap-pinned across the
  // session for fast re-use.  Pre-fix it stayed pinned even after the
  // user switched books, eating ~25 KB of heap baseline for the next
  // book's chapter parse + cache cold-extend pressure window.  Concrete
  // hardware symptom (v2.0.177-178 logs): FB2 spine=46 cold-extend with
  // an inline image hit `largest=8180` because the JPEG instance from
  // the previously-read Valley-of-Genius EPUB cover was still alive.
  //
  // Existing release callsites (line 2912 after cover-gen, ReaderState
  // Overlays.cpp:739 / PageCache.cpp:986 reactive on heap-critical) all
  // fire AFTER the damage is done.  Releasing here on book exit gives
  // the next book a clean ~25 KB of recovered heap from the start,
  // before any of its allocations begin.
  //
  // Cost: next book's first JPEG-bearing operation (cover decode or
  // inline image) pays ~10 ms to re-allocate the JPEGDEC instance —
  // negligible compared to the 50-200 ms typical JPEG decode time.
  //
  // No-op when SNAPIX_SMOL_JPEG=1 (v3_alpha env never instantiates
  // JPEGDEC), so this is free in that build.
  //
  // Safety: ReaderState::exit() runs AFTER asyncJobs_.stopWorker() above
  // (line 1015), so no concurrent JPEG decode can be in flight when we
  // free the instance.
  JpegToBmpConverter::releaseAllPersistent();

  // v2.0.190 — RE-INTRODUCING v2.0.183 lifecycle hygiene cache clears
  // now that v2.0.189 has eliminated the latent TXT/ParsedText heap
  // corruption these clears originally exposed.
  //
  // BACKGROUND — the v2.0.183 → 185 → 189 → 190 arc:
  //   * v2.0.183 added these three unconditional clears to recover an
  //     additional ~6-23 KB per book switch, on top of v2.0.180's JPEG
  //     release.
  //   * v2.0.185 reverted them after a `multi_heap_free` poisoning
  //     assert in the TXT cold-extend ParsedText spill path
  //     (multi_heap_poisoning.c:279 (head != NULL)).  The crash was
  //     in code v2.0.183 didn't touch, but the new clears changed the
  //     allocator's free-block layout enough to make a pre-existing
  //     latent bug fatal.
  //   * v2.0.189 traced the root cause: PlainTextParser was reading
  //     the original cp1251 SD source directly (bypassing the
  //     v2.0.187 UTF-8 cache) and feeding the resulting invalid
  //     UTF-8 stream into `utf8NormalizeNfc()`.  NFC normalization
  //     walks the input as UTF-8 codepoints and writes
  //     contraction/decomposition results back to the same buffer;
  //     malformed continuation byte sequences caused it to walk past
  //     the buffer end and overwrite the next heap block's metadata.
  //     With v2.0.189 the parser reads the validated UTF-8 cache
  //     and the normalization stays within bounds.
  //   * v2.0.190 — root cause gone → re-add the v2.0.183 clears.
  //     Hardware verification on the same Asimov TXT (royallib.ru
  //     cp1251, 14 pages read) showed zero `multi_heap_free` asserts
  //     after v2.0.189.
  //
  // The three caches being cleared (rationale unchanged from v2.0.183):
  //
  //   1. ImageRenderCache (renderer_.imageCache(), unordered_map<string,
  //      Entry>).  Each entry is ~3-10 KB of decoded bitmap; v2.0.52
  //      comment at line ~3030 notes the cache can pin "30+ KB of
  //      contiguous heap" after image-bearing books.  Reactive clear
  //      at line ~3035 fires only when isHeapCritical() trips in the
  //      BG worker — too late for the NEXT book's first parse, which
  //      already inherited the fragmented heap.
  //
  //   2. Font streaming bitmap caches
  //      (FONT_MANAGER.clearStreamingBitmapCaches()).  Per-glyph
  //      bitmap pool for the open .epdfont reader.  Reactive call at
  //      ReaderStateOverlays.cpp:740 fires on page-fill trim; the
  //      entry-time call at line ~820 (enter()) re-clears before each
  //      book's first render.  Adding it here on exit ensures the
  //      bitmaps don't pin heap during the FB2 cold-extend window
  //      between books, before enter() has a chance to clear them.
  //
  //   3. Glyph advance-width cache (renderer_.clearWidthCache()).
  //      String→advance memoization built up during text layout.
  //      Reactive call at line ~821 (next-book entry-time) re-clears
  //      it, but the cache stays heap-resident during the exit-to-
  //      entry transition.
  //
  // Combined estimated win: ~5-20 KB recovered per book switch, on
  // top of v2.0.180's 25 KB from JpegToBmpConverter::releaseAllPersistent
  // — total ~31-49 KB per book-switch recovery vs the pre-v2.0.180
  // baseline.  All three calls are no-ops when the underlying cache
  // is empty, so safe to call unconditionally on every book exit.
  //
  // ZipFile::fileStatSlimCache was considered but skipped: ZipFile is
  // already constructed transiently (`ZipFile zip(filepath)` at each
  // openItemStream / readItemContentsToStream callsite — see
  // Epub.cpp:605, :820, :1014).  The cache dies with its enclosing
  // ZipFile instance on every operation; no persistent state survives
  // between book sessions to clean up.
  //
  // Safety: ReaderState::exit() runs AFTER asyncJobs_.stopWorker()
  // above, so no concurrent reader/parser/render is in flight when
  // these clears execute.
  renderer_.imageCache().clear();
  FONT_MANAGER.clearStreamingBitmapCaches();
  renderer_.clearWidthCache();

  // v2.0.194 — additional hygiene targets identified by the v2.0.193
  // hardware-log audit (heap fragmented at ~9 KB largest block during
  // TXT cold-extend, blocking background cache extension).
  //
  // 1. Bitmap row buffers (~2.6 KB pinned by GfxRenderer after first
  //    BMP render).  Pre-v2.0.194 only freed in the GfxRenderer dtor
  //    (i.e. never during runtime).  Lazy-realloc on next BMP render
  //    is cheap (~µs).
  renderer_.freeBitmapRowBuffers();

  // 2. streamOffsetCache_ capacity (v3_alpha builds only).  clear() at
  //    line ~1046 / ~2669 sheds elements but vector::capacity stays.
  //    For a 36-page EPUB chapter that's 36 * 8 B (PageBoundarySnapshot)
  //    = 288 B + vector struct overhead ~24 B = ~312 B pinned across
  //    book switches.  shrink_to_fit forces re-allocation to actual
  //    size (which is 0 after clear), releasing the capacity.
  //
  //    Note: we don't clear here — that's already done above.  Just
  //    shed the empty-but-allocated capacity.
#if defined(SNAPIX_MARKERIZED_RENDER) && SNAPIX_MARKERIZED_RENDER
  if (streamOffsetCache_.capacity() > 0 && streamOffsetCache_.empty()) {
    streamOffsetCache_.shrink_to_fit();
  }
#endif

  // Keep the active .epdfont reader family loaded across UI transitions.
  // Several UI surfaces reuse theme.readerFontId directly, so unloading the
  // family here leaves stale font IDs in the renderer path and causes
  // "Font <id> not found" logs after leaving the reader. The large external
  // CJK fallback font is still safe to release.
  FONT_MANAGER.unloadExternalFont();

  // Reset overlay modes that may have been active when exit was triggered
  menuMode_ = false;
  bookmarkMode_ = false;
  tocMode_ = false;

  contentLoaded_ = false;
  contentPath_[0] = '\0';
  cacheController_.setContentPath(contentPath_);
  activeCore_ = nullptr;
  directUiTransition_ = false;
  resumeBackgroundCachingAfterRender_ = false;
  lastIdleBackgroundKickMs_ = 0;
  asyncJobs_.clearPendingTocJump();
  clearPendingEpubPageLoad();
  invalidateAnchorMapCache();

  // Reset orientation to Portrait for UI
  renderer_.setOrientation(GfxRenderer::Orientation::Portrait);
}

StateTransition ReaderState::update(Core& core) {
  // Handle load failure - transition to error state or back to file list
  if (loadFailed_ || !contentLoaded_) {
    // If error message was set, show ErrorState; otherwise just go back to FileList
    if (core.buf.text[0] != '\0') {
      return StateTransition::to(StateId::Error);
    }
    return StateTransition::to(StateId::FileList);
  }

  if (pendingTocJumpActive_ && !needsRender_) {
    processPendingTocJump(core);
    if (asyncJobs_.navigationJobBlocksInput()) {
      return StateTransition::stay(StateId::Reader);
    }
  }

  if (pendingEpubPageLoadActive_ && !needsRender_) {
    processPendingEpubPageLoad(core);
    Event pendingEvent;
    while (core.events.pop(pendingEvent)) {
      if (pendingEvent.type == EventType::ButtonPress || pendingEvent.type == EventType::ButtonRelease ||
          pendingEvent.type == EventType::ButtonRepeat) {
        lastReaderInteractionMs_ = millis();
      }
      if (pendingEvent.type == EventType::ButtonPress && pendingEvent.button == Button::Back) {
        return exitToUI(core);
      }

      if (pendingEvent.type == EventType::ButtonPress && pendingEvent.button == Button::Power &&
          core.settings.shortPwrBtn == Settings::PowerPageTurn) {
        powerPressStartedMs_ = millis();
        continue;
      }

      if (pendingEvent.type != EventType::ButtonRelease) {
        continue;
      }

      switch (pendingEvent.button) {
        case Button::Right:
        case Button::Down:
          enqueuePendingPageTurn(1, "pending-epub-page-load");
          break;
        case Button::Left:
        case Button::Up:
          enqueuePendingPageTurn(-1, "pending-epub-page-load");
          break;
        case Button::Power:
          if (core.settings.shortPwrBtn == Settings::PowerPageTurn && powerPressStartedMs_ != 0) {
            const uint32_t heldMs = millis() - powerPressStartedMs_;
            if (heldMs < core.settings.getPowerButtonDuration()) {
              enqueuePendingPageTurn(1, "pending-epub-page-load");
            }
          }
          powerPressStartedMs_ = 0;
          break;
        default:
          break;
      }
    }
    return StateTransition::stay(StateId::Reader);
  }

  const bool navigationBlocksInput = asyncJobs_.navigationJobBlocksInput();
  if (queuedPendingEpubTurn_ != 0 && !needsRender_ && !navigationBlocksInput && !pendingEpubPageLoadActive_ &&
      !menuMode_ && !bookmarkMode_ && !tocMode_) {
    int queuedTurn = 0;
    uint32_t queuedForMs = 0;
    asyncJobs_.noteQueuedTurnWorkerIdle(isWorkerRunning());
    if (asyncJobs_.tryConsumeQueuedTurn(isWorkerRunning(), needsRender_, navigationBlocksInput,
                                        pendingEpubPageLoadActive_, menuMode_, bookmarkMode_, tocMode_, queuedTurn,
                                        queuedForMs)) {
      LOG_INF(TAG, "[INPUT] executing deferred page-turn dir=%d wait=%lu remaining=%d", queuedTurn,
              static_cast<unsigned long>(queuedForMs), queuedPendingEpubTurn_);

      if (queuedTurn > 0) {
        navigateNext(core);
      } else {
        navigatePrev(core);
      }
      return StateTransition::stay(StateId::Reader);
    }
  }

  int pendingRefreshSpine = -1;
  int pendingRefreshPage = -1;
  uint32_t pendingRefreshToken = 0;
  if (asyncJobs_.pendingRefresh().snapshot(pendingRefreshSpine, pendingRefreshPage,
                                           pendingRefreshToken)) {
    if (currentSpineIndex_ != pendingRefreshSpine ||
        currentSectionPage_ != pendingRefreshPage) {
      asyncJobs_.pendingRefresh().clearIfUnchanged(pendingRefreshToken);
    } else if (!needsRender_ && !pendingTocJumpActive_ && !pendingEpubPageLoadActive_ && !menuMode_ &&
               !bookmarkMode_ && !tocMode_) {
      // Don't gate on !isWorkerRunning(): after wake from deep sleep the worker
      // loads the current cache from disk (~20ms) then continues with extend +
      // prefetch (~seconds).  If we wait for the worker to fully finish, the
      // page won't render until the prefetch completes.  The cache file is
      // already written; a concurrent worker poses no risk to the reader.
      if (isWorkerRunning()) {
        stopBackgroundCaching();
        resumeBackgroundCachingAfterRender_ = true;
      }
      LOG_INF(TAG, "[CACHE] refreshing current page after background cache rewrite spine=%d page=%d",
              currentSpineIndex_, currentSectionPage_);
      asyncJobs_.pendingRefresh().clearIfUnchanged(pendingRefreshToken);
      needsRender_ = true;
    }
  }

  Event e;
  while (core.events.pop(e)) {
    if (pendingTocJumpActive_ && pendingTocJumpDeferredDisplay_ &&
        reader::cancelsDeferredTocFollowup(e)) {
      LOG_INF(TAG,
              "[ASYNC] cancelling deferred TOC follow-up due to actionable "
              "input type=%u button=%u",
              static_cast<unsigned>(e.type),
              static_cast<unsigned>(e.button));
      asyncJobs_.clearPendingTocJump();
      if (isWorkerRunning()) {
        requestWorkerCancel();
      }
    }
    if (e.type == EventType::ButtonPress || e.type == EventType::ButtonRelease || e.type == EventType::ButtonRepeat) {
      lastReaderInteractionMs_ = millis();
    }
    if (menuMode_) {
      handleMenuInput(core, e);
      continue;
    }
    if (bookmarkMode_) {
      handleBookmarkInput(core, e);
      continue;
    }
    if (tocMode_) {
      handleTocInput(core, e);
      continue;
    }

    switch (e.type) {
      case EventType::ButtonPress:
        switch (e.button) {
          case Button::Right:
          case Button::Down:
          case Button::Left:
          case Button::Up:
            if (isWorkerRunning()) {
              asyncJobs_.markCachePreemptRequested(millis());
              LOG_INF(TAG, "[INPUT] preempt requested button=%d workerState=%d", static_cast<int>(e.button),
                      static_cast<int>(workerState()));
              requestWorkerCancel();
            }
            break;
          case Button::Center:
            enterMenuMode(core);
            break;
          case Button::Back:
            return exitToUI(core);
          case Button::Power:
            if (core.settings.shortPwrBtn == Settings::PowerPageTurn) {
              powerPressStartedMs_ = millis();
              if (isWorkerRunning()) {
                asyncJobs_.markCachePreemptRequested(millis());
                LOG_INF(TAG, "[INPUT] preempt requested button=%d workerState=%d", static_cast<int>(e.button),
                        static_cast<int>(workerState()));
                requestWorkerCancel();
              }
            }
            break;
          default:
            break;
        }
        break;

      case EventType::ButtonRepeat:
        if (!holdNavigated_) {
          switch (e.button) {
            case Button::Right:
            case Button::Down:
              navigateNextChapter(core);
              holdNavigated_ = true;
              break;
            case Button::Left:
            case Button::Up:
              navigatePrevChapter(core);
              holdNavigated_ = true;
              break;
            default:
              break;
          }
        }
        break;

      case EventType::ButtonRelease:
        if (!holdNavigated_) {
          switch (e.button) {
            case Button::Right:
            case Button::Down:
              LOG_INF(TAG, "[INPUT] page-turn release button=%d workerActive=%u", static_cast<int>(e.button),
                      static_cast<unsigned>(isWorkerRunning()));
              navigateNext(core);
              break;
            case Button::Left:
            case Button::Up:
              LOG_INF(TAG, "[INPUT] page-turn release button=%d workerActive=%u", static_cast<int>(e.button),
                      static_cast<unsigned>(isWorkerRunning()));
              navigatePrev(core);
              break;
            case Button::Power:
              if (core.settings.shortPwrBtn == Settings::PowerPageTurn && powerPressStartedMs_ != 0) {
                const uint32_t heldMs = millis() - powerPressStartedMs_;
                if (heldMs < core.settings.getPowerButtonDuration()) {
                  LOG_INF(TAG, "[INPUT] page-turn release button=%d workerActive=%u", static_cast<int>(e.button),
                          static_cast<unsigned>(isWorkerRunning()));
                  navigateNext(core);
                }
              }
              break;
            default:
              break;
          }
        }
        if (e.button == Button::Power) {
          powerPressStartedMs_ = 0;
        }
        holdNavigated_ = false;
        break;

      default:
        break;
    }
  }

  if (!needsRender_ && !isWorkerRunning() && !pendingTocJumpActive_ && !pendingEpubPageLoadActive_ && !menuMode_ &&
      !bookmarkMode_ && !tocMode_) {
    const uint32_t nowMs = millis();
    if (lastIdleBackgroundKickMs_ == 0 || nowMs - lastIdleBackgroundKickMs_ >= kIdleBackgroundKickIntervalMs) {
      lastIdleBackgroundKickMs_ = nowMs;
      startBackgroundCaching(core, "idle");
    }
  }

  return StateTransition::stay(StateId::Reader);
}

void ReaderState::render(Core& core) {
  if (!needsRender_ || !contentLoaded_) {
    return;
  }

  const bool overlayRender = menuMode_ || bookmarkMode_ || tocMode_;
  if (overlayRender && isWorkerRunning()) {
    stopBackgroundCaching();
  }

  if (pendingTocJumpActive_ && !pendingTocJumpDeferredDisplay_) {
    if (!pendingTocJumpIndexingShown_) {
      // v2.0.199 — unified status banner uses "Loading..." for every
      // wait state.  Previously this branch showed "Indexing..." for the
      // TOC-jump path; user preferred a single label.
      renderLoadingStatusMessage(core);
      pendingTocJumpIndexingShown_ = true;
    }
  } else if (pendingTocJumpActive_ && pendingTocJumpDeferredDisplay_) {
    // v2.0.84: optimistic TOC navigation — render the target spine's page
    // 0 (or current target page) right away while processPendingTocJump
    // continues looking for the exact anchor in the background.  When the
    // anchor resolves, it'll mutate currentSectionPage_ and re-render.
    renderCurrentPage(core);
    if (resumeBackgroundCachingAfterRender_) {
      resumeBackgroundCachingAfterRender_ = false;
      if (!isWorkerRunning()) {
        startBackgroundCaching(core, "resume-after-toc-deferred-render");
      }
    }
  } else if (pendingEpubPageLoadActive_) {
    if (!pendingEpubPageLoadMessageShown_) {
      // v2.0.199 — unified "Loading..." (was conditional Indexing/Loading).
      renderLoadingStatusMessage(core);
      pendingEpubPageLoadMessageShown_ = true;
    }
  } else if (menuMode_) {
    const Theme& theme = THEME_MANAGER.current();
    ui::render(renderer_, theme, menuView_);
    core.display.markDirty();
  } else if (bookmarkMode_) {
    renderBookmarkOverlay(core);
  } else if (tocMode_) {
    renderTocOverlay(core);
  } else {
    renderCurrentPage(core);
    if (!pendingEpubPageLoadActive_) {
      lastRenderedSpineIndex_ = currentSpineIndex_;
      lastRenderedSectionPage_ = currentSectionPage_;
    }
    if (resumeBackgroundCachingAfterRender_) {
      resumeBackgroundCachingAfterRender_ = false;
      if (!isWorkerRunning()) {
        startBackgroundCaching(core, "resume-after-render");
      }
    }
  }

  needsRender_ = false;
}

void ReaderState::navigateNext(Core& core) {
  // v2.0.84: user-initiated navigation cancels deferred-display TOC follow-up.
  // The user has committed to reading at their current page; if the BG TOC
  // worker resolves the anchor later, we don't want to yank them out of place.
  if (pendingTocJumpActive_ && pendingTocJumpDeferredDisplay_) {
    LOG_INF(TAG, "[ASYNC] cancelling deferred TOC jump (user navigated forward)");
    asyncJobs_.clearPendingTocJump();
  }

  ContentType type = core.content.metadata().type;

  // XTC uses flatPage navigation, not spine/section - skip to navigation logic
  if (type == ContentType::Xtc) {
    stopBackgroundCaching();
    ReaderNavigation::Position pos;
    pos.flatPage = currentPage_;
    auto result = ReaderNavigation::next(type, pos, nullptr, core.content.pageCount());
    applyNavResult(result, core);
    return;
  }

  if (tryFastNavigateNext(core)) {
    return;
  }

  // Page-turns no longer defer behind background work.  Instead, we ask the
  // worker to abort and proceed immediately — every long-running phase
  // (base64 stream, JPEG/PNG decode, parser inner loop) honours
  // `shouldAbort` and bails within ~10-100 ms, so `stopBackgroundCaching()`
  // below returns quickly.  Any image whose decode was preempted shows the
  // "Loading image..." placeholder on the new page until the BG worker
  // reawakens (post-render or idle trigger) and produces the next stage of
  // the progressive preview chain.
  if (isWorkerRunning()) {
    requestWorkerCancel();
  }

  // Spine/section logic for EPUB, TXT, Markdown
  // From cover (-1) -> first text content page
  if (currentSpineIndex_ == 0 && currentSectionPage_ == -1) {
    stopBackgroundCaching();
    auto* provider = core.content.asEpub();
    size_t spineCount = 1;
    if (provider && provider->getEpub()) {
      spineCount = provider->getEpub()->getSpineItemsCount();
    }
    int firstContentSpine = calcFirstContentSpine(hasCover_, textStartIndex_, spineCount);

    if (firstContentSpine != currentSpineIndex_) {
      currentSpineIndex_ = firstContentSpine;
      parser_.reset();
      parserSpineIndex_ = -1;
      if (lookaheadParserSpineIndex_ != currentSpineIndex_) {
        clearLookaheadParser();
      }
      pageCache_.reset();
      invalidateAnchorMapCache();
      clearPagePrefetch();
      resetBackgroundPrefetchState();
    }
    currentSectionPage_ = 0;
    needsRender_ = true;
    resumeBackgroundCachingAfterRender_ = false;
    return;
  }

  if (tryFastNavigateWithinCurrentCache(core, 1)) {
    return;
  }

  stopBackgroundCaching();

  if (type != ContentType::Epub && pageCache_ && pageCache_->pageCount() == 0) {
    const int targetPage = std::max(currentSectionPage_ + 1, 0);
    LOG_INF(TAG, "[NAV] next recovering from empty active cache spine=%d targetPage=%d", currentSpineIndex_,
            targetPage);
    pageCache_.reset();
    invalidateAnchorMapCache();
    clearPagePrefetch();
    armPendingEpubPageLoad(core, currentSpineIndex_, targetPage, false, false);
    needsRender_ = true;
    resumeBackgroundCachingAfterRender_ = false;
    return;
  }

  ReaderNavigation::Position pos;
  pos.spineIndex = currentSpineIndex_;
  pos.sectionPage = currentSectionPage_;
  pos.flatPage = currentPage_;
  const uint32_t navTotal = (type == ContentType::Fb2) ? core.content.tocCount() : core.content.pageCount();
  auto result = ReaderNavigation::next(type, pos, pageCache_.get(), navTotal);
  if (!result.needsRender && result.position.spineIndex == pos.spineIndex && result.position.sectionPage == pos.sectionPage &&
      result.position.flatPage == pos.flatPage) {
    LOG_DBG(TAG, "[NAV] next no-op type=%d spine=%d page=%d cache=%u pages=%u partial=%u", static_cast<int>(type),
            currentSpineIndex_, currentSectionPage_, static_cast<unsigned>(pageCache_ != nullptr),
            static_cast<unsigned>(pageCache_ ? pageCache_->pageCount() : 0),
            static_cast<unsigned>(pageCache_ ? pageCache_->isPartial() : 0));
  }
  applyNavResult(result, core);
}

bool ReaderState::tryFastNavigateNext(Core& core) {
  (void)core;

  if (currentSectionPage_ < 0) {
    return false;
  }

  const int targetPage = currentSectionPage_ + 1;
  if (!warmedNextPage_.matches(currentSpineIndex_, targetPage)) {
    return false;
  }

  renderOverridePage_ = warmedNextPage_;
  warmedNextPage_ = warmedNextNextPage_;
  warmedNextNextPage_.clear();

  currentSectionPage_ = targetPage;
  clearPendingEpubPageLoad();
  needsRender_ = true;
  resumeBackgroundCachingAfterRender_ = false;

  LOG_DBG(TAG, "Fast next-page turn using detached warm page spine=%d page=%d", currentSpineIndex_, currentSectionPage_);
  return true;
}

bool ReaderState::tryFastNavigateWithinCurrentCache(Core& core, const int direction) {
  (void)core;

  if (isWorkerRunning() || !pageCache_) {
    return false;
  }

  const int targetPage = currentSectionPage_ + direction;
  const int pageCount = static_cast<int>(pageCache_->pageCount());
  if (targetPage < 0 || targetPage >= pageCount) {
    return false;
  }

  currentSectionPage_ = targetPage;
  clearPendingEpubPageLoad();
  needsRender_ = true;
  resumeBackgroundCachingAfterRender_ = false;

  LOG_DBG(TAG, "Fast cached page turn using active cache spine=%d page=%d dir=%d", currentSpineIndex_,
          currentSectionPage_, direction);
  return true;
}

void ReaderState::navigatePrev(Core& core) {
  // v2.0.84: same cancel rule as navigateNext — user navigation kills the
  // deferred TOC follow-up so we don't override their position.
  if (pendingTocJumpActive_ && pendingTocJumpDeferredDisplay_) {
    LOG_INF(TAG, "[ASYNC] cancelling deferred TOC jump (user navigated back)");
    asyncJobs_.clearPendingTocJump();
  }

  ContentType type = core.content.metadata().type;

  // XTC uses flatPage navigation, not spine/section - skip to navigation logic
  if (type == ContentType::Xtc) {
    stopBackgroundCaching();
    ReaderNavigation::Position pos;
    pos.flatPage = currentPage_;
    auto result = ReaderNavigation::prev(type, pos, nullptr);
    applyNavResult(result, core);
    return;
  }

  // Spine/section logic for EPUB, TXT, Markdown
  auto* provider = core.content.asEpub();
  size_t spineCount = 1;
  if (provider && provider->getEpub()) {
    spineCount = provider->getEpub()->getSpineItemsCount();
  }
  int firstContentSpine = calcFirstContentSpine(hasCover_, textStartIndex_, spineCount);

  // Prevent going back from cover
  if (currentSpineIndex_ == 0 && currentSectionPage_ == -1) {
    startBackgroundCaching(core, "nav-prev-cover");  // Resume task before returning
    return;                        // Already at cover
  }

  // See navigateNext: always proceed; cancel any running worker so the rest
  // of the prev-navigation path (and stopBackgroundCaching below) sees an
  // idle worker quickly.  Worker phases honour shouldAbort so the wait is
  // short.
  if (isWorkerRunning()) {
    requestWorkerCancel();
  }

  // At first page of text content
  if (currentSpineIndex_ == firstContentSpine && currentSectionPage_ == 0) {
    // Only go to cover if it exists and images enabled
    if (hasCover_ && core.settings.showImages) {
      stopBackgroundCaching();
      currentSpineIndex_ = 0;
      currentSectionPage_ = -1;
      parser_.reset();
      parserSpineIndex_ = -1;
      clearLookaheadParser();
      pageCache_.reset();  // Don't need cache for cover
      invalidateAnchorMapCache();
      clearPagePrefetch();
      resetBackgroundPrefetchState();
      needsRender_ = true;
    }
    return;  // At start of book either way
  }

  if (tryFastNavigateWithinCurrentCache(core, -1)) {
    return;
  }

  stopBackgroundCaching();

  if (type != ContentType::Epub && pageCache_ && pageCache_->pageCount() == 0 && currentSectionPage_ > 0) {
    const int targetPage = std::max(currentSectionPage_ - 1, 0);
    LOG_INF(TAG, "[NAV] prev recovering from empty active cache spine=%d targetPage=%d", currentSpineIndex_,
            targetPage);
    pageCache_.reset();
    invalidateAnchorMapCache();
    clearPagePrefetch();
    armPendingEpubPageLoad(core, currentSpineIndex_, targetPage, false, false);
    needsRender_ = true;
    resumeBackgroundCachingAfterRender_ = false;
    return;
  }

  ReaderNavigation::Position pos;
  pos.spineIndex = currentSpineIndex_;
  pos.sectionPage = currentSectionPage_;
  pos.flatPage = currentPage_;
  auto result = ReaderNavigation::prev(type, pos, pageCache_.get());
  if (!result.needsRender && result.position.spineIndex == pos.spineIndex && result.position.sectionPage == pos.sectionPage &&
      result.position.flatPage == pos.flatPage) {
    LOG_DBG(TAG, "[NAV] prev no-op type=%d spine=%d page=%d cache=%u pages=%u partial=%u", static_cast<int>(type),
            currentSpineIndex_, currentSectionPage_, static_cast<unsigned>(pageCache_ != nullptr),
            static_cast<unsigned>(pageCache_ ? pageCache_->pageCount() : 0),
            static_cast<unsigned>(pageCache_ ? pageCache_->isPartial() : 0));
  }
  applyNavResult(result, core);
}

void ReaderState::applyNavResult(const ReaderNavigation::NavResult& result, Core& core) {
  const int previousSpineIndex = currentSpineIndex_;
  currentSpineIndex_ = result.position.spineIndex;
  currentSectionPage_ = result.position.sectionPage;
  currentPage_ = result.position.flatPage;
  if (core.content.metadata().type == ContentType::Fb2) {
    currentPage_ = currentSectionPage_;
  }
  // Use |= so a no-op navigation (needsRender=false) doesn't cancel a
  // pre-existing render request.  This is critical on wake from deep sleep:
  // enter() sets needsRender_=true, but the wake button can fire a spurious
  // page-turn that produces a no-op NavResult before the first render runs.
  needsRender_ |= result.needsRender;
  clearPendingEpubPageLoad();
  if (result.needsCacheReset) {
    parser_.reset();  // Safe - task already stopped by caller
    parserSpineIndex_ = -1;
    if (lookaheadParserSpineIndex_ != currentSpineIndex_) {
      clearLookaheadParser();
    }
    pageCache_.reset();
    invalidateAnchorMapCache();
    clearPagePrefetch();
    resetBackgroundPrefetchState();
  } else if (currentSpineIndex_ != previousSpineIndex) {
    resetBackgroundPrefetchState();
  }
  if (result.needsRender) {
    resumeBackgroundCachingAfterRender_ = false;
  } else {
    startBackgroundCaching(core, "nav-no-render");  // Resume caching when no visible page render is pending
  }
}

void ReaderState::navigateNextChapter(Core& core) {
  ContentType type = core.content.metadata().type;

  if (type == ContentType::Xtc) {
    const uint16_t count = core.content.tocCount();
    if (count == 0) return;

    // Find current chapter
    int currentChapter = -1;
    for (uint16_t i = 0; i < count; i++) {
      auto result = core.content.getTocEntry(i);
      if (result.ok() && result.value.pageIndex <= currentPage_) {
        currentChapter = i;
      }
    }

    if (currentChapter + 1 >= static_cast<int>(count)) return;

    auto next = core.content.getTocEntry(currentChapter + 1);
    if (!next.ok()) return;

    currentPage_ = next.value.pageIndex;
    needsRender_ = true;
    return;
  }

  if (type == ContentType::Fb2) {
    const uint16_t count = core.content.tocCount();
    if (count == 0 || currentSpineIndex_ + 1 >= static_cast<int>(count)) return;

    stopBackgroundCaching();
    currentSpineIndex_++;
    currentSectionPage_ = 0;
    parser_.reset();
    parserSpineIndex_ = -1;
    clearLookaheadParser();
    pageCache_.reset();
    invalidateAnchorMapCache();
    clearPagePrefetch();
    resetBackgroundPrefetchState();
    needsRender_ = true;
    resumeBackgroundCachingAfterRender_ = false;
    return;
  }

  if (type != ContentType::Epub) return;

  auto* provider = core.content.asEpub();
  if (!provider || !provider->getEpub()) return;

  size_t spineCount = provider->getEpub()->getSpineItemsCount();
  if (currentSpineIndex_ + 1 >= static_cast<int>(spineCount)) return;

  stopBackgroundCaching();
  currentSpineIndex_++;
  currentSectionPage_ = 0;
  parser_.reset();
  parserSpineIndex_ = -1;
  if (lookaheadParserSpineIndex_ != currentSpineIndex_) {
    clearLookaheadParser();
  }
  pageCache_.reset();
  invalidateAnchorMapCache();
  clearPagePrefetch();
  resetBackgroundPrefetchState();
  needsRender_ = true;
  resumeBackgroundCachingAfterRender_ = false;
}

void ReaderState::navigatePrevChapter(Core& core) {
  ContentType type = core.content.metadata().type;

  if (type == ContentType::Xtc) {
    const uint16_t count = core.content.tocCount();
    if (count == 0) return;

    // Find current chapter
    int currentChapter = -1;
    uint32_t currentChapterStart = 0;
    for (uint16_t i = 0; i < count; i++) {
      auto result = core.content.getTocEntry(i);
      if (result.ok() && result.value.pageIndex <= currentPage_) {
        currentChapter = i;
        currentChapterStart = result.value.pageIndex;
      }
    }

    if (currentChapter < 0) return;

    if (currentPage_ > currentChapterStart) {
      // Mid-chapter: go to start of current chapter
      currentPage_ = currentChapterStart;
    } else if (currentChapter > 0) {
      // At start of chapter: go to previous chapter
      auto prev = core.content.getTocEntry(currentChapter - 1);
      if (!prev.ok()) return;
      currentPage_ = prev.value.pageIndex;
    } else {
      return;
    }

    needsRender_ = true;
    return;
  }

  if (type == ContentType::Fb2) {
    stopBackgroundCaching();

    if (currentSectionPage_ > 0) {
      currentSectionPage_ = 0;
    } else {
      if (currentSpineIndex_ <= 0) {
        startBackgroundCaching(core, "fb2-prev-chapter-boundary");
        return;
      }
      currentSpineIndex_--;
      currentSectionPage_ = 0;
      parser_.reset();
      parserSpineIndex_ = -1;
      clearLookaheadParser();
      pageCache_.reset();
      invalidateAnchorMapCache();
      clearPagePrefetch();
      resetBackgroundPrefetchState();
    }

    needsRender_ = true;
    resumeBackgroundCachingAfterRender_ = false;
    return;
  }

  if (type != ContentType::Epub) return;

  stopBackgroundCaching();

  if (currentSectionPage_ > 0) {
    // Go to beginning of current chapter
    currentSectionPage_ = 0;
  } else {
    // Go to previous chapter
    auto* provider = core.content.asEpub();
    size_t spineCount = 1;
    if (provider && provider->getEpub()) {
      spineCount = provider->getEpub()->getSpineItemsCount();
    }
    int firstContentSpine = calcFirstContentSpine(hasCover_, textStartIndex_, spineCount);
    if (currentSpineIndex_ <= firstContentSpine) {
      startBackgroundCaching(core, "prev-chapter-boundary");
      return;
    }
    currentSpineIndex_--;
    currentSectionPage_ = 0;
    parser_.reset();
    parserSpineIndex_ = -1;
    clearLookaheadParser();
    pageCache_.reset();
    invalidateAnchorMapCache();
    clearPagePrefetch();
    resetBackgroundPrefetchState();
  }

  needsRender_ = true;
  resumeBackgroundCachingAfterRender_ = false;
}

void ReaderState::renderCurrentPage(Core& core) {
  ContentType type = core.content.metadata().type;
  const Theme& theme = THEME_MANAGER.current();

  if (type == ContentType::Epub && !pendingTocJumpActive_ && pendingTocJumpRetryCount_ > 0) {
    crashdebug::mark(crashdebug::CrashPhase::EpubTocRender, static_cast<int16_t>(currentSpineIndex_),
                     pendingTocJumpRetryCount_);
  }

  // v2.0.202 — clearScreen MOVED INTO each render-bearing branch below.
  // Previously eager clearScreen at function entry wiped the framebuffer
  // unconditionally — including in the page-load-deferred paths that
  // return early without rendering anything.  This left a blank
  // framebuffer for the subsequent renderCenteredStatusMessage banner
  // overlay, defeating its "overlay on previous content" design (user
  // saw banner on blank white instead of banner over the TOC/file
  // browser that was visible just before).
  //
  // New rule: only clear when we're committed to actually drawing a
  // full-screen frame.  Deferred paths leave the framebuffer alone so
  // the previous content (TOC menu, file browser, page) stays under
  // the loading banner that the render() loop draws next.

  // Cover page: spineIndex=0, sectionPage=-1 (only when showImages enabled)
  if (currentSpineIndex_ == 0 && currentSectionPage_ == -1) {
    if (core.settings.showImages) {
      // Cover render takes over the full screen — clear before drawing.
      renderer_.clearScreen(theme.backgroundColor);
      if (renderCoverPage(core)) {
        hasCover_ = true;
        core.display.markDirty();
        return;
      }
      // No cover - skip spine 0 if textStartIndex is 0 (likely empty cover document)
      hasCover_ = false;
      currentSectionPage_ = 0;
      if (textStartIndex_ == 0) {
        // Only skip to spine 1 if it exists
        auto* provider = core.content.asEpub();
        if (provider && provider->getEpub()) {
          const auto* epub = provider->getEpub();
          if (epub->getSpineItemsCount() > 1) {
            currentSpineIndex_ = 1;
          }
        }
      }
      // Fall through to render content
    } else {
      currentSectionPage_ = 0;
    }
  }

  switch (type) {
    case ContentType::Epub:
    case ContentType::Txt:
    case ContentType::Markdown:
    case ContentType::Fb2:
    case ContentType::Html:
      renderCachedPage(core);
      break;
    case ContentType::Xtc:
      renderXtcPage(core);
      break;
    default:
      break;
  }

  if (!pendingTocJumpActive_ && !pendingEpubPageLoadActive_ && !isWorkerRunning()) {
    startBackgroundCaching(core, "post-render");
  }

  if (type == ContentType::Epub && !pendingTocJumpActive_ && pendingTocJumpRetryCount_ > 0) {
    crashdebug::clear();
    pendingTocJumpRetryCount_ = 0;
  }

  core.display.markDirty();
}

void ReaderState::renderCachedPage(Core& core) {
  const uint32_t totalStartMs = reader::perfMsNow();
  Theme& theme = THEME_MANAGER.mutableCurrent();
  ContentType type = core.content.metadata().type;
  const auto vp = getReaderViewport(core.settings.statusBar != 0);

  // Handle EPUB bounds
  if (type == ContentType::Epub) {
    auto* provider = core.content.asEpub();
    if (!provider || !provider->getEpub()) return;

    auto epub = provider->getEpubShared();
    if (currentSpineIndex_ < 0) currentSpineIndex_ = 0;
    if (currentSpineIndex_ >= static_cast<int>(epub->getSpineItemsCount())) {
      // v2.0.202 — "End of book" needs an explicit clearScreen now that
      // renderCurrentPage no longer eagerly clears.  Without it the
      // text would draw over the previous framebuffer content (TOC,
      // file list, etc.).
      renderer_.clearScreen(theme.backgroundColor);
      renderer_.drawCenteredText(core.settings.getReaderFontId(theme), 300, "End of book", theme.primaryTextBlack,
                                 BOLD);
      renderer_.displayBuffer();
      return;
    }
  } else if (type == ContentType::Fb2) {
    auto* provider = core.content.asFb2();
    if (fb2UsesSectionNavigation(provider)) {
      const int tocCount = static_cast<int>(provider->tocCount());
      if (tocCount <= 0) {
        return;
      }
      if (currentSpineIndex_ < 0) {
        currentSpineIndex_ = 0;
      } else if (currentSpineIndex_ >= tocCount) {
        currentSpineIndex_ = tocCount - 1;
        currentSectionPage_ = 0;
      }
    }
  }

  if (renderOverridePage_.matches(currentSpineIndex_, currentSectionPage_)) {
    WarmPageSlot pageSlot = renderOverridePage_;
    renderOverridePage_.clear();
    LOG_DBG(TAG, "Rendered via detached warm page spine=%d page=%d", currentSpineIndex_, currentSectionPage_);
    renderLoadedPage(core, pageSlot.page, pageSlot.pageCount, pageSlot.isPartial, theme, vp, totalStartMs,
                     !isWorkerRunning(), true);
    return;
  }

  // v2.0.110 (audit fix #1): use the short-wait interactive cancel instead
  // of `stopBackgroundCaching()` here.  Pre-fix this called the full
  // teardown variant which waits up to 15 s for the worker to idle and
  // ESP.restart()s on timeout.  In practice the worker's cooperative
  // cancel completes in ~100-300 ms for healthy paths; the 15 s ceiling
  // was provisioned for heavy paths (huge JPEG decode, deeply nested
  // parse).  On the render path that ceiling translates directly into
  // "buttons unresponsive while the worker drains" — `loopTask` is
  // blocked inside `xEventGroupWaitBits` and never gets to poll input.
  // 500 ms is enough for healthy cancel; on timeout we proceed against
  // the on-disk cache (worker writes use .rebuild → atomic rename, so
  // the .bin reader sees a consistent snapshot regardless).
  if (isWorkerRunning()) {
    if (!requestBackgroundCachingPause("render-cached-page")) {
      // The worker still owns parser/page-cache state. Keep the previous
      // framebuffer and retry later instead of racing it.
      needsRender_ = true;
      resumeBackgroundCachingAfterRender_ = true;
      return;
    }
  }

  auto foregroundResources = cacheController_.acquireForegroundResources("render-cached-page");
  if (!foregroundResources) {
    needsRender_ = true;
    return;
  }

  // Background task may have left parser in inconsistent state
  if (!pageCache_ && parser_ && parserSpineIndex_ == currentSpineIndex_ && !parser_->canResume()) {
    parser_.reset();
    parserSpineIndex_ = -1;
  }

  // Create or load cache if needed
  if (!pageCache_) {
    const uint32_t cacheBootstrapMs = reader::perfMsNow();
    // Try to load existing cache silently first
    loadCacheFromDisk(core);

    if (pageCache_ && currentSectionPage_ == INT16_MAX && !pageCache_->isPartial() && pageCache_->pageCount() > 0) {
      currentSectionPage_ = static_cast<int>(pageCache_->pageCount()) - 1;
    }

    bool pageIsCached =
        pageCache_ && currentSectionPage_ >= 0 && currentSectionPage_ < static_cast<int>(pageCache_->pageCount());

    if (!pageIsCached) {
      const bool requireComplete = currentSectionPage_ == INT16_MAX;
      const int targetPage = requireComplete ? 0 : std::max(currentSectionPage_, 0);

      // v2.0.112 (lifehack — instant page show on book entry):
      //
      // Pre-fix: when user resumes a book at e.g. saved page=7 but cache has
      // only 4 pages (0..3), we'd arm page-load + show "Loading..." banner
      // for the full cold rebuild (15-25 s on heavy chapters).  The user
      // sees a frozen banner with no content.
      //
      // New: if the cache has ANY pages and the user's saved page is past
      // the cached range, show the LAST CACHED PAGE immediately and remember
      // the original target.  Background cache-extend continues; when it
      // produces enough pages to cover the target, `processPendingEpubPageLoad`
      // upgrade path (the existing v2.0.103 deferred-display logic) will
      // navigate forward to the saved position.  Until then user reads real
      // content instead of staring at "Loading".
      //
      // requireComplete=true (INT16_MAX sentinel = "end of section") still
      // takes the original deferred path because there's no meaningful
      // "show closest" — we need the full cache to know the END.
      if (!requireComplete && pageCache_ && pageCache_->pageCount() > 0 &&
          targetPage >= static_cast<int>(pageCache_->pageCount())) {
        const int lastCached = static_cast<int>(pageCache_->pageCount()) - 1;
        LOG_INF(TAG,
                "[ASYNC] instant-show last cached spine=%d savedPage=%d shownPage=%d cachedPages=%u (bg extend → target)",
                currentSpineIndex_, targetPage, lastCached,
                static_cast<unsigned>(pageCache_->pageCount()));
        currentSectionPage_ = lastCached;
        // Arm page-load WITHOUT the "indexing=true" banner — BG extend
        // continues silently.  When cachedPages > targetPage,
        // processPendingEpubPageLoad's deferred-display upgrade picks
        // up the navigation forward.
        armPendingEpubPageLoad(core, currentSpineIndex_, targetPage, false, false);
        // Don't return — fall through to the rest of renderCachedPage
        // (cache load, page load, render).  Now pageIsCached is true.
      } else {
        LOG_INF(TAG, "[ASYNC] deferring cache build spine=%d page=%d complete=%u type=%d", currentSpineIndex_,
                targetPage, static_cast<unsigned>(requireComplete), static_cast<int>(type));
        armPendingEpubPageLoad(core, currentSpineIndex_, targetPage, requireComplete, true);
        return;
      }
    }
    readerPerfLog("reader-cache-bootstrap", cacheBootstrapMs, "(spine=%d page=%d)", currentSpineIndex_, currentSectionPage_);

    // Re-validate page against current cache state.  A background cold extend
    // may have replaced the cache file between the pageIsCached check above
    // and this point (a UART yield inside readerPerfLog is the window).
    // If the page is now out-of-range, treat it as uncached and defer rather
    // than silently clamping (which would jump the user backward).
    if (pageCache_) {
      const int cachedPages = static_cast<int>(pageCache_->pageCount());
      if (currentSectionPage_ < 0) {
        currentSectionPage_ = 0;
      } else if (currentSectionPage_ >= cachedPages) {
        LOG_INF(TAG, "[NAV] page out-of-range after cache change spine=%d page=%d cachedPages=%d partial=%u",
                currentSpineIndex_, currentSectionPage_, cachedPages,
                static_cast<unsigned>(pageCache_->isPartial()));
        if (pageCache_->isPartial()) {
          // Cache is still growing — request the missing page instead of clamping.
          armPendingEpubPageLoad(core, currentSpineIndex_, currentSectionPage_, false, false);
          return;
        }
        // Cache is complete.  The user's page truly doesn't exist
        // (section shrank after a rebuild).  Move to the last valid page.
        currentSectionPage_ = cachedPages > 0 ? cachedPages - 1 : 0;
      }
    }
  }

  // Check if we need to extend cache
  if (!ensurePageCached(core, currentSectionPage_)) {
    if (pendingEpubPageLoadActive_) {
      return;
    }
    LOG_ERR(TAG, "[NAV] page unavailable spine=%d page=%d cache=%u pages=%u partial=%u", currentSpineIndex_,
            currentSectionPage_, static_cast<unsigned>(pageCache_ != nullptr),
            static_cast<unsigned>(pageCache_ ? pageCache_->pageCount() : 0),
            static_cast<unsigned>(pageCache_ ? pageCache_->isPartial() : 0));
    armPendingEpubPageLoad(core, currentSpineIndex_, currentSectionPage_, false, true);
    resumeBackgroundCachingAfterRender_ = false;
    needsRender_ = true;
    return;
  }

  // Load and render page (cache is now guaranteed to exist, we own it)
  size_t pageCount = pageCache_ ? pageCache_->pageCount() : 0;
  const uint32_t pageLoadMs = reader::perfMsNow();
  std::shared_ptr<Page> page = pageCache_ ? pageCache_->loadPage(currentSectionPage_) : nullptr;
  readerPerfLog("reader-page-load", pageLoadMs, "(spine=%d page=%d)", currentSpineIndex_, currentSectionPage_);

  if (!page) {
    LOG_ERR(TAG, "[NAV] cached page unreadable; clearing cache and recovering dir structure");
    if (pageCache_) {
      pageCache_->clear();
      pageCache_.reset();
    }
    // v2.0.136 CRITICAL — reset the parser too.  Pre-fix, the parser's
    // R4.b short-circuit progress (shortCircuitNextPage_ in
    // EpubChapterParser/Fb2Parser) survived cache-clear-on-read-error.
    // The next parsePages call would emit pages starting from where the
    // parser left off (e.g. paginator-page 10) while writing them to a
    // FRESH cache file at indices 0..N.  Reader requests page 3 → cache
    // returns the page stored at file-index 3 → that's actually
    // paginator-page 13.  Visible as "text doesn't connect between
    // pages" (each page shows content from a totally different position
    // in the chapter).
    //
    // parser->reset() drops shortCircuitActive_, clears initialized_,
    // and releases the live legacy parser if any.  Next parsePages
    // re-enters the init path which short-circuits cleanly from page 0.
    // Markerize/idx files on disk are reused (markerizeAttempted_ flag
    // is cleared by reset too — but the actual idx file's existence
    // is what gates re-running tryMarkerizeChapter and R4.c).
    if (parser_) {
      parser_->reset();
    }
    if (lookaheadParser_) {
      lookaheadParser_->reset();
    }
    // Re-create cache directory hierarchy — SdFat can lose directory entries
    // under memory pressure, causing all subsequent cache operations to fail.
    if (core.content.metadata().type == ContentType::Epub) {
      auto* provider = core.content.asEpub();
      if (provider && provider->getEpub()) {
        provider->getEpub()->setupCacheDir();
      }
    } else if (core.content.metadata().type == ContentType::Fb2) {
      auto* provider = core.content.asFb2();
      if (provider && provider->getFb2()) {
        provider->getFb2()->setupCacheDir();
      }
    }
    invalidateAnchorMapCache();
    clearPagePrefetch();
    armPendingEpubPageLoad(core, currentSpineIndex_, currentSectionPage_, false, true);
    resumeBackgroundCachingAfterRender_ = false;
    needsRender_ = true;
    return;
  }

  renderLoadedPage(core, page, pageCount, pageCache_ ? pageCache_->isPartial() : false, theme, vp, totalStartMs, true);
}

void ReaderState::renderLoadedPage(Core& core, const std::shared_ptr<Page>& page, const size_t pageCount,
                                   const bool cacheIsPartial, const Theme& theme, const Viewport& vp,
                                   const uint32_t totalStartMs, const bool allowPagePrefetch,
                                   const bool pageGlyphsWarm) {
  renderer_.clearScreen(theme.backgroundColor);

  const int fontId = core.settings.getReaderFontId(theme);
  const bool aaEnabled = core.settings.textAntiAliasing && renderer_.fontSupportsGrayscale(fontId);

  TextBlock::bionicReading = core.settings.bionicReading;
  TextBlock::fakeBold = core.settings.fakeBold;
  if (!pageGlyphsWarm) {
    const uint32_t glyphWarmMs = reader::perfMsNow();
    page->warmGlyphs(renderer_, fontId);
    // v2.0.67: also warm status-bar font glyphs for the book title.  The
    // status bar uses theme.statusFontId (typically different from the
    // reader fontId, so it has its OWN glyph cache).  On a cold cache —
    // first render after entering reader, when streaming-font caches were
    // just cleared — the title's Cyrillic/CJK codepoints all miss and
    // each one triggers a font-file read, which is what makes initial
    // reader-render-bw spike to 720 ms-8 s on long-Russian-titled books.
    // Pre-warming is cheap (single batched call) and folds into the
    // glyph-warm phase that's already on the perf budget.
    if (core.settings.statusBar != Settings::StatusNone) {
      const char* title = core.content.metadata().title;
      if (title && *title) {
        renderer_.warmTextGlyphs(theme.statusFontId, title);
      }
      // If chapter-mode is on AND we have a cached chapter title from a
      // previous renderStatusBar call, warm it too.  Don't recompute via
      // findCurrentTocEntry here — that's a TOC lookup we don't want to
      // pay twice; renderStatusBar itself caches and reuses across renders.
      if (core.settings.statusBar == Settings::StatusChapter && cachedChapterTitle_[0] != '\0') {
        renderer_.warmTextGlyphs(theme.statusFontId, cachedChapterTitle_);
      }
    }
    readerPerfLog("reader-glyph-warm", glyphWarmMs, nullptr);
  }

  const uint32_t renderBwMs = reader::perfMsNow();
  renderPageContents(core, *page, vp.marginTop, vp.marginRight, vp.marginBottom, vp.marginLeft);
  renderStatusBar(core, vp.marginRight, vp.marginBottom, vp.marginLeft, static_cast<int>(pageCount), cacheIsPartial);
  readerPerfLog("reader-render-bw", renderBwMs, "(aa=%u images=%u)", static_cast<unsigned>(aaEnabled),
          static_cast<unsigned>(page->hasImages()));

  const uint32_t displayMs = reader::perfMsNow();
  displayWithRefresh(core);
  readerPerfLog("reader-display-main", displayMs, nullptr);

  // Grayscale text rendering (anti-aliasing)
  if (aaEnabled) {
    const uint32_t aaMs = reader::perfMsNow();
    renderer_.clearScreen(0x00);
    renderer_.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page->render(renderer_, fontId, vp.marginLeft, vp.marginTop, theme.primaryTextBlack);
    renderStatusBar(core, vp.marginRight, vp.marginBottom, vp.marginLeft, static_cast<int>(pageCount),
                    cacheIsPartial);
    renderer_.copyGrayscaleLsbBuffers();

    renderer_.clearScreen(0x00);
    renderer_.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page->render(renderer_, fontId, vp.marginLeft, vp.marginTop, theme.primaryTextBlack);
    renderStatusBar(core, vp.marginRight, vp.marginBottom, vp.marginLeft, static_cast<int>(pageCount),
                    cacheIsPartial);
    renderer_.copyGrayscaleMsbBuffers();

    const bool turnOffScreen = core.settings.sunlightFadingFix != 0;
    renderer_.displayGrayBuffer(turnOffScreen);
    renderer_.setRenderMode(GfxRenderer::BW);

    // Re-render BW instead of restoring from backup (saves 48KB peak allocation)
    renderer_.clearScreen(theme.backgroundColor);
    renderPageContents(core, *page, vp.marginTop, vp.marginRight, vp.marginBottom, vp.marginLeft);
    renderStatusBar(core, vp.marginRight, vp.marginBottom, vp.marginLeft, static_cast<int>(pageCount),
                    cacheIsPartial);
    renderer_.cleanupGrayscaleWithFrameBuffer();
    readerPerfLog("reader-aa-pass", aaMs, nullptr);
  }

  const uint32_t prefetchMs = reader::perfMsNow();
  if (allowPagePrefetch) {
    prefetchAdjacentPage(core);
    readerPerfLog("reader-prefetch", prefetchMs, nullptr);
  } else {
    readerPerfLog("reader-prefetch-skip", prefetchMs, "(reason=detached-fast-turn)");
  }

  LOG_DBG(TAG, "Rendered page %d/%d", currentSectionPage_ + 1, pageCount);
  readerPerfLog("reader-total", totalStartMs, "(spine=%d page=%d)", currentSpineIndex_, currentSectionPage_);
  // Count actual visible page presentation as reader activity so far-idle
  // sweeps do not relaunch immediately on a stale input timestamp.
  lastReaderInteractionMs_ = millis();
  lastIdleBackgroundKickMs_ = millis();
}

bool ReaderState::ensurePageCached(Core& core, uint16_t pageNum) {
  if (!pageCache_) {
    return false;
  }

  const reader::HeapState heap = reader::readHeapState();
  if (reader::isHeapCritical(heap)) {
    pageCache_->clearResidentPages();
  } else if (reader::isHeapTight(heap)) {
    pageCache_->trimResidentPages(pageNum, 0, 2);
  }

  size_t pageCount = pageCache_->pageCount();
  const bool needsExtension = pageCache_->needsExtension(pageNum);
  const bool isPartial = pageCache_->isPartial();

  if (pageNum < pageCount) {
    if (needsExtension) {
      LOG_DBG(TAG, "Pre-extending cache at page %d", pageNum);
      if (!isWorkerRunning()) {
        startBackgroundCaching(core, "pre-extend");
      }
    }
    return true;
  }

  if (!isPartial) {
    LOG_DBG(TAG, "Page %d not available (cache complete at %d pages)", pageNum, static_cast<int>(pageCount));
    return false;
  }

  LOG_DBG(TAG, "Extending cache for page %d", pageNum);

  if (core.content.metadata().type == ContentType::Epub) {
    LOG_INF(TAG, "[ASYNC] deferring EPUB cache extend spine=%d page=%u cachedPages=%u partial=%u", currentSpineIndex_,
            static_cast<unsigned>(pageNum), static_cast<unsigned>(pageCount), static_cast<unsigned>(isPartial));
    armPendingEpubPageLoad(core, currentSpineIndex_, pageNum, false, false);
    return false;
  }

  armPendingEpubPageLoad(core, currentSpineIndex_, pageNum, false, false);
  return false;
}

void ReaderState::loadCacheFromDisk(Core& core) {
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  cacheController_.loadCacheFromDisk(core, vp);
}

void ReaderState::reloadCacheFromDisk(Core& core) {
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  cacheController_.reloadCacheFromDisk(core, vp);
}

void ReaderState::createOrExtendCache(Core& core, uint16_t batchSize) {
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  cacheController_.createOrExtendCache(core, vp, batchSize);
  invalidateAnchorMapCache();
}

void ReaderState::renderPageContents(Core& core, Page& page, int marginTop, int marginRight, int marginBottom,
                                     int marginLeft) {
  (void)marginRight;
  (void)marginBottom;

  const Theme& theme = THEME_MANAGER.current();
  const int fontId = core.settings.getReaderFontId(theme);
  TextBlock::bionicReading = core.settings.bionicReading;
  TextBlock::fakeBold = core.settings.fakeBold;

#if defined(SNAPIX_MARKERIZED_RENDER) && SNAPIX_MARKERIZED_RENDER
  // v2.0.122 Phase R3.5 — try the v3 streaming render path.
  // v2.0.127 Phase R3.7 — now supports both EPUB and FB2.
  //
  // Resolve markers path per content type:
  //   * EPUB: `<epubCachePath>/markers/<currentSpineIndex_>.bin`
  //   * FB2:  `<fb2CachePath>/markers/<sectionIndex>.bin`, where
  //          `sectionIndex` is `Fb2::TocItem::sectionIndex` looked
  //          up via `currentSpineIndex_` (== tocIndex for FB2).
  //
  // Falls back to the legacy Page-tree path on any failure (markers
  // file missing, PageNotFound, ReadError, content type not supported).
  bool streamedOK = false;
  // v2.0.167 — markers + idx now live as UnifiedCache segments in
  // <bookCachePath>/streaming.cache.  Build the cache instance + segment
  // key (uint16) per content type.  EPUB key = spineIndex; FB2 key =
  // sectionIndex (which differs from tocIndex via Fb2::TocItem).
  std::string bookCachePath;
  int markersKey = -1;
  // v3.9.0 — single-section docs (TXT/MD/no-TOC FB2) store markers across
  // consecutive chunk segments (keys 0..N) for lazy/progressive markerize;
  // section docs (EPUB/FB2-with-TOC) use one segment keyed by sectionIndex.
  bool markersSingleSection = false;
  const ContentType contentType = core.content.metadata().type;
  if (contentType == ContentType::Epub) {
    auto* epubProv = core.content.asEpub();
    if (epubProv && epubProv->getEpub() && currentSpineIndex_ >= 0) {
      bookCachePath = epubProv->getEpub()->getCachePath();
      markersKey = currentSpineIndex_;
    }
  } else if (contentType == ContentType::Fb2) {
    auto* fb2Prov = core.content.asFb2();
    if (fb2Prov && fb2Prov->getFb2()) {
      auto* fb2 = fb2Prov->getFb2();
      if (fb2->tocCount() == 0) {
        // v3.8.0 — TOC-less FB2 is one whole-book streamed section (key 0),
        // exactly like TXT/MD.  (Was the legacy ParsedText/Expat render path.)
        bookCachePath = fb2->getCachePath();
        markersKey = 0;
        markersSingleSection = true;  // v3.9.0 — chunked markers
      } else if (currentSpineIndex_ >= 0 &&
                 currentSpineIndex_ < static_cast<int>(fb2->tocCount())) {
        const Fb2::TocItem item = fb2->getTocItem(static_cast<uint16_t>(currentSpineIndex_));
        if (item.sectionIndex >= 0) {
          bookCachePath = fb2->getCachePath();
          markersKey = item.sectionIndex;
        }
      }
    }
  } else if (contentType == ContentType::Txt) {
    // v3.7.0 — TXT is single-section: markers + idx live under key 0.
    auto* txtProv = core.content.asTxt();
    if (txtProv && txtProv->getTxt()) {
      bookCachePath = txtProv->getTxt()->getCachePath();
      markersKey = 0;
      markersSingleSection = true;  // v3.9.0 — chunked markers
    }
  } else if (contentType == ContentType::Markdown) {
    // v3.7.0 — Markdown is single-section: markers + idx live under key 0.
    auto* mdProv = core.content.asMarkdown();
    if (mdProv && mdProv->getMarkdown()) {
      bookCachePath = mdProv->getMarkdown()->getCachePath();
      markersKey = 0;
      markersSingleSection = true;  // v3.9.0 — chunked markers
    }
  } else if (contentType == ContentType::Html) {
    auto* htmlProv = core.content.asHtml();
    if (htmlProv && htmlProv->getHtml()) {
      bookCachePath = htmlProv->getHtml()->getCachePath();
      markersKey = 0;
      markersSingleSection = true;
    }
  }
  if (!bookCachePath.empty() && markersKey >= 0) {
    auto ucache = snapix::unifiedcache::UnifiedCache::shared(bookCachePath);
    // v3.9.0 — read markers through ChunkedMarkersReader so a single-section
    // doc's progressively-written chunk segments (keys 0..N) present as ONE
    // logical stream with global offsets.  A 1-chunk doc (every doc today, plus
    // every EPUB/FB2 section) behaves exactly like the old single-segment read.
    auto chunkProvider = markersSingleSection
        ? snapix::pagecache::UnifiedCacheChunkProvider::singleSection(ucache)
        : snapix::pagecache::UnifiedCacheChunkProvider::section(
              ucache, static_cast<uint16_t>(markersKey));
    snapix::smolport::ChunkedMarkersReader markersReader(chunkProvider);
    const size_t markersSegSize = markersReader.totalSize();
    {
      if (markersReader.chunkCount() > 0 && markersSegSize > 0) {
          // Build paginator config from current viewport + font metrics.
          // Page geometry intentionally mirrors GfxRenderer's logical
          // screen so layout decisions match the legacy Page tree's
          // assumptions.  Heading uses 1.5× line height; arbitrary —
          // R3.6 will tune against the legacy renderer's heading metrics.
          // v3.5.1 — apply the user's line-spacing (lineCompression) to the
          // body line height.  The v3 streaming path previously used the raw
          // font height, so the Line Spacing setting had NO effect on EPUB/FB2
          // (only the legacy ParsedText path honoured it).  MUST match the
          // MEASURE-walk config in EpubChapterParser/Fb2Parser (config_.
          // lineCompression) — both derive from settings.getLineCompression().
          const float lineComp = core.settings.getLineCompression();
          const uint16_t bodyLineH =
              static_cast<uint16_t>(renderer_.getLineHeight(fontId) * lineComp);
          snapix::smolport::StreamingPaginatorConfig cfg{};
          cfg.pageWidth = static_cast<uint16_t>(renderer_.getScreenWidth());
          cfg.pageHeight = static_cast<uint16_t>(renderer_.getScreenHeight());
          cfg.marginTop = static_cast<uint16_t>(marginTop);
          cfg.marginBottom = static_cast<uint16_t>(marginBottom);
          cfg.marginLeft = static_cast<uint16_t>(marginLeft);
          cfg.marginRight = static_cast<uint16_t>(marginRight);
          cfg.bodyLineHeight = bodyLineH > 0 ? bodyLineH : 24;
          cfg.headingLineHeight = static_cast<uint16_t>(cfg.bodyLineHeight * 3 / 2);
          // v3.5.2 — Text Layout (Compact/Standard/Large) drives paragraph
          // spacing + first-line indent (it was hardcoded before, so the
          // setting had no effect on EPUB/FB2).  MUST match the MEASURE-walk
          // cfg in EpubChapterParser/Fb2Parser — both use the same settings.
          cfg.paragraphSpacing = snapix::smolport::paragraphSpacingForLevel(
              core.settings.getSpacingLevel(), cfg.bodyLineHeight);
          cfg.firstLineIndent = snapix::smolport::firstLineIndentForLevel(
              core.settings.getIndentLevel(), cfg.bodyLineHeight);
          // v3.3.0 — hyphenation language.  MUST match the MEASURE-walk cfg in
          // EpubChapterParser (EPUB declared language) / Fb2Parser ("ru") or
          // page boundaries drift between the .idx build and this render.
          // TXT/MD/standalone HTML have no declared document language, so they disable
          // hyphenation (hyphenLang ""), matching the MEASURE-walk config in
          // StreamingSection::ensureStreamingSectionIdx.  cfg.hyphenate is part
          // of the .idx configHash, so MEASURE and DRAW MUST agree here.
          const bool hasNoDeclaredLanguage =
              contentType == ContentType::Txt || contentType == ContentType::Markdown ||
              contentType == ContentType::Html;
          cfg.hyphenate = !hasNoDeclaredLanguage;
          {
            const char* lang = "ru";  // FB2 default
            if (contentType == ContentType::Epub && core.content.asEpub() &&
                core.content.asEpub()->getEpub()) {
              lang = core.content.asEpub()->getEpub()->getLanguage().c_str();
            } else if (hasNoDeclaredLanguage) {
              lang = "";
            }
            std::strncpy(cfg.hyphenLang, lang, sizeof(cfg.hyphenLang) - 1);
            cfg.hyphenLang[sizeof(cfg.hyphenLang) - 1] = '\0';
          }

          // v2.0.136 — diagnostic: dump R3.6 paginator config so a
          // mismatch with R4.c's idx-build cfg becomes immediately
          // visible in the log.  Compare against `[STREAM] R4.c
          // paginator cfg` lines from EpubChapterParser / Fb2Parser.
          LOG_INF(TAG,
                  "[STREAM] R3.6 paginator cfg spine=%d type=%s fontId=%d "
                  "pageW=%u pageH=%u mT=%u mB=%u mL=%u mR=%u bodyLH=%u",
                  currentSpineIndex_,
                  streamContentTypeName(contentType), fontId,
                  static_cast<unsigned>(cfg.pageWidth), static_cast<unsigned>(cfg.pageHeight),
                  static_cast<unsigned>(cfg.marginTop), static_cast<unsigned>(cfg.marginBottom),
                  static_cast<unsigned>(cfg.marginLeft), static_cast<unsigned>(cfg.marginRight),
                  static_cast<unsigned>(cfg.bodyLineHeight));

          // v2.0.145 — image resolver: marker stream's `<img src>` payload
          // → cached BMP path on LittleFS.  Uses the same path convention
          // as EpubImageCache::cacheImage:
          //   bmpPath = <imageCachePath>/<std::hash(resolvedPath)>.bmp
          // where resolvedPath = FsHelpers::normalisePath(chapterBase + src).
          // The legacy parser populates these BMPs during chapter parse;
          // we just consume them here.  FB2 sections use a similar
          // convention via the Fb2 provider.  Returns empty string if
          // the content type doesn't support inline images (yet) or
          // the path can't be resolved.
          std::string imageCacheDir;
          std::string chapterBase;
          if (contentType == ContentType::Epub) {
            auto* epubProv = core.content.asEpub();
            if (epubProv && epubProv->getEpub() && currentSpineIndex_ >= 0) {
              imageCacheDir = epubProv->getEpub()->getCachePath() + "/images";
              // Chapter base path = directory of the spine item's href
              // (e.g., "OEBPS/Text/" for "OEBPS/Text/chapter01.xhtml").
              const auto spineItem =
                  epubProv->getEpub()->getSpineItem(static_cast<uint16_t>(currentSpineIndex_));
              const size_t lastSlash = spineItem.href.rfind('/');
              if (lastSlash != std::string::npos) {
                chapterBase = spineItem.href.substr(0, lastSlash + 1);
              }
            }
          } else if (contentType == ContentType::Fb2) {
            auto* fb2Prov = core.content.asFb2();
            if (fb2Prov && fb2Prov->getFb2()) {
              imageCacheDir = fb2Prov->getFb2()->getCachePath() + "/images";
              // FB2 image refs are bare binary-ids (no chapter-relative
              // path needed); chapterBase stays empty.
            }
          }
          // v2.0.147/148 — image resolvers for FB2 + EPUB.  Both use
          // the same lazy-decode-on-demand pattern: if the cached BMP
          // doesn't exist on LittleFS yet, decode the source image
          // (FB2 binary block / EPUB ZIP entry) → write BMP → return
          // path.  Idempotent on subsequent calls (cached BMP is just
          // re-opened).  Lets the streaming render pipeline display
          // images without running the full legacy parser.
          Fb2* fb2ForImages = nullptr;
          std::shared_ptr<Epub> epubForImages;
          if (contentType == ContentType::Fb2) {
            auto* fb2Prov = core.content.asFb2();
            if (fb2Prov) fb2ForImages = fb2Prov->getFb2();
          } else if (contentType == ContentType::Epub) {
            auto* epubProv = core.content.asEpub();
            if (epubProv) epubForImages = epubProv->getEpubShared();
          }
          const uint16_t imgMaxW = static_cast<uint16_t>(renderer_.getScreenWidth() -
                                                           marginLeft - marginRight);
          const uint16_t imgMaxH = static_cast<uint16_t>(renderer_.getScreenHeight() -
                                                           marginTop - marginBottom);
          // v2.0.148 — EPUB lazy image cache.  Construct a temp
          // EpubImageCache instance with the minimal setup
          // needed for `cacheImageForStreaming` (no parse loop,
          // no Page tree build).  Wraps the chapter's readItemFn
          // so JPEG/PNG decode can pull the source from the EPUB
          // ZIP exactly the way the legacy parser would.  Capture
          // by value so the lambda's lifetime spans the render.
          std::shared_ptr<EpubImageCache> epubImageCacheParser;
          if (epubForImages && !imageCacheDir.empty() &&
              currentSpineIndex_ >= 0) {
            auto readItemFn = [epubForImages](
                                  const std::string& href, Print& out, size_t chunkSize,
                                  const std::function<bool()>& localAbort)
                -> EpubImageCache::ReadItemStatus {
              // v2.0.152 — DO NOT pass `renderer_.getFrameBuffer()` as the
              // ZIP-extract scratch buffer here.  The legacy EpubChapter
              // Parser does that in ReaderAsync where the framebuffer is
              // idle, but the streaming resolver runs INSIDE the render
              // path (loopTask, mid-page-draw) — using the framebuffer
              // as ZIP scratch overwrites the pixels being sent to the
              // display, producing vertical-stripe garbage on the right
              // 2/3 of the screen (left side stays correct because
              // text-draw happens before the image-cache call).
              //
              // Passing nullptr makes Epub::readItemContentsToStreamDetailed
              // allocate its own scratch from heap — one-shot, freed
              // when the call returns.  Slightly slower (allocation
              // overhead per image decode) but correct.
              switch (epubForImages->readItemContentsToStreamDetailed(
                  href, out, chunkSize, /*scratch=*/nullptr, localAbort)) {
                case Epub::ItemReadResult::Success:
                  return EpubImageCache::ReadItemStatus::Success;
                case Epub::ItemReadResult::Aborted:
                  return EpubImageCache::ReadItemStatus::Aborted;
                case Epub::ItemReadResult::NotFound:
                  return EpubImageCache::ReadItemStatus::NotFound;
                case Epub::ItemReadResult::WriteError:
                  return EpubImageCache::ReadItemStatus::WriteError;
                case Epub::ItemReadResult::ArchiveError:
                  return EpubImageCache::ReadItemStatus::ArchiveError;
                case Epub::ItemReadResult::IoError:
                case Epub::ItemReadResult::OpenFailed:
                  return EpubImageCache::ReadItemStatus::IoError;
              }
              return EpubImageCache::ReadItemStatus::ArchiveError;
            };
            // Build RenderConfig from the streaming paginator's
            // viewport — RenderConfig's only fields cacheImage cares
            // about are viewportWidth/Height, which gate the max
            // image bounds during the BMP scale-down pass.
            const auto vpForImg = getReaderViewport(core.settings.statusBar != 0);
            const RenderConfig rcfg = core.settings.getRenderConfig(theme, vpForImg.width, vpForImg.height);
            // v2.0.171 — EpubImageCache ctor trimmed to just the params
            // it actually uses (renderer, config, chapterBase, imageCachePath,
            // readItemFn, quickImageDecode).  Parser-era args dropped.
            // Queue source extraction and conversion instead of performing
            // either inside the UI render.  ReaderAsync processes only images
            // encountered on the visible page and requests repaint.
            epubImageCacheParser = std::make_shared<EpubImageCache>(
                renderer_, rcfg, chapterBase, imageCacheDir, readItemFn,
                /*quickImageDecode=*/true);
          }
          auto resolvedImages =
              std::make_shared<std::vector<std::pair<std::string, std::string>>>();
          auto resolveImage = [imageCacheDir, chapterBase, contentType,
                                fb2ForImages, epubImageCacheParser, imgMaxW,
                                imgMaxH, resolvedImages](const uint8_t* p,
                                                        size_t l) -> std::string {
            if (p == nullptr || l == 0) return {};
            std::string src(reinterpret_cast<const char*>(p), l);
            const std::string cacheKey = src;
            for (const auto& cached : *resolvedImages) {
              if (cached.first == cacheKey) return cached.second;
            }
            if (contentType == ContentType::Fb2 && fb2ForImages != nullptr) {
              // FB2: strip leading `#` from `<image l:href="#id">`.
              if (!src.empty() && src[0] == '#') src.erase(0, 1);
              std::string outPath;
              uint16_t w = 0, h = 0;
              const bool ok = fb2ForImages->cacheImage(
                  src, outPath, w, h, imgMaxW, imgMaxH,
                  /*fastMode=*/true, /*shouldAbort=*/{});
              resolvedImages->emplace_back(cacheKey,
                                           ok ? outPath : std::string());
              return ok ? outPath : std::string();
            }
            if (contentType == ContentType::Epub && epubImageCacheParser) {
              // If an earlier pass already queued this image, prioritise it
              // without decoding synchronously inside the UI render.  The old
              // drainTarget() call blocked the visible page for 3-10 seconds.
              const std::string resolved =
                  FsHelpers::normalisePath(chapterBase + src);
              const std::string expectedPath =
                  imageCacheDir + "/" +
                  std::to_string(std::hash<std::string>{}(resolved)) + ".bmp";
              if (snapix::pendingImage::isPendingOrActive(expectedPath)) {
                (void)snapix::pendingImage::promote(expectedPath);
                resolvedImages->emplace_back(cacheKey, expectedPath);
                return expectedPath;
              }

              std::string outPath;
              uint16_t w = 0, h = 0;
              const bool ok = epubImageCacheParser->cacheImageForStreaming(
                  src, outPath, w, h);
              resolvedImages->emplace_back(cacheKey,
                                           ok ? outPath : std::string());
              return ok ? outPath : std::string();
            }
            // No resolver available — return empty so paginator skips.
            return {};
          };

          // v2.0.146 — propagate user's fakeBold setting so the
          // streaming render path matches the legacy Page-tree's
          // bold-via-multi-pass behaviour.  Settings:
          //   0 = off (use real bold font)
          //   1 = bold (2× draw at x, x+1 with REGULAR/ITALIC)
          //   2 = extrabold (3× draw at x-1, x, x+1)
          snapix::smolport::GfxRendererPaginatorAdapter adapter(renderer_, fontId, fontId,
                                                                  theme.primaryTextBlack,
                                                                  resolveImage,
                                                                  core.settings.fakeBold,
                                                                  core.settings.getSuperSubFontId(theme));  // v3.6.0
          snapix::smolport::StreamingPaginator paginator(cfg, adapter);

          // Streaming chunk buffer on stack — same 4 KB sizing as the
          // markerize path; ReaderAsync stack high water is ~12 KB
          // observed, leaves ~4 KB headroom.
          constexpr size_t kChunkBufBytes = 4096;
          uint8_t chunkBuf[kChunkBufBytes];

          // v2.0.125 R3.6 — clear per-spine offset cache when chapter
          // changes (different markers file → different page offsets).
          // v2.0.128 R4.a — on spine change, ALSO persist the previous
          // spine's cache to its `.idx` file (if non-empty), and try
          // to load the new spine's `.idx` from disk to avoid the
          // MEASURE-only walk from scratch.
          if (streamOffsetCacheSpine_ != currentSpineIndex_) {
            // Persist outgoing spine's cache (R4.a save side).
            if (streamOffsetCacheSpine_ >= 0 && !streamOffsetCache_.empty() &&
                !streamOffsetCacheBookPath_.empty() && streamOffsetCacheKey_ >= 0) {
              const uint16_t prevHash = streamOffsetCacheConfigHash_;
              const size_t needed =
                  snapix::smolport::kPageIndexHeaderBytes +
                  streamOffsetCache_.size() * snapix::smolport::kPageIndexEntryBytes;
              if (needed <= 4096) {  // sanity cap; ~500 pages fits
                uint8_t serdebuf[4096];
                const size_t wrote = snapix::smolport::serializePageIndex(
                    streamOffsetCache_.data(), streamOffsetCache_.size(), prevHash,
                    serdebuf, sizeof(serdebuf));
                if (wrote > 0) {
                  // v2.0.167 — write idx to UnifiedCache::Idx segment instead
                  // of separate .idx file with .work + rename dance.
                  auto prevCache = snapix::unifiedcache::UnifiedCache::shared(streamOffsetCacheBookPath_);
                  if (prevCache.writeSegment(snapix::unifiedcache::Kind::Idx,
                                              static_cast<uint16_t>(streamOffsetCacheKey_),
                                              serdebuf, wrote)) {
                    LOG_INF(TAG, "[STREAM] idx saved spine=%d entries=%u (UnifiedCache::Idx key=%d)",
                            streamOffsetCacheSpine_,
                            static_cast<unsigned>(streamOffsetCache_.size()),
                            streamOffsetCacheKey_);
                  }
                }
              }
            }

            streamOffsetCache_.clear();
            streamOffsetCacheSpine_ = currentSpineIndex_;

            // Try to load incoming spine's .idx (R4.a load side).
            const uint16_t cfgHash =
                snapix::smolport::computePageIndexConfigHash(cfg, fontId,
                                                              core.settings.fakeBold);
            streamOffsetCacheConfigHash_ = cfgHash;
            streamOffsetCacheBookPath_ = bookCachePath;
            streamOffsetCacheKey_ = markersKey;
            // v2.0.167 — read idx from UnifiedCache::Idx segment.
            // v3.7.0 — cap raised 4 KB → 64 KB: a single TXT/MD section can run
            // to thousands of pages (idx = 8 B/page), and idxPayload is a heap
            // vector so the larger read is safe.  Per-chapter EPUB/FB2 idx stay
            // small; this only lets the big single-section idx load.
            std::vector<uint8_t> idxPayload;
            if (ucache.readSegment(snapix::unifiedcache::Kind::Idx,
                                     static_cast<uint16_t>(markersKey), idxPayload)) {
              if (idxPayload.size() > 0 && idxPayload.size() <= 65536 &&
                  snapix::smolport::deserializePageIndex(idxPayload.data(), idxPayload.size(),
                                                          cfgHash, streamOffsetCache_)) {
                LOG_INF(TAG, "[STREAM] idx loaded spine=%d entries=%u (UnifiedCache::Idx)",
                        currentSpineIndex_,
                        static_cast<unsigned>(streamOffsetCache_.size()));
              } else {
                // Stale (config mismatch) or corrupt — tombstone + start fresh.
                ucache.removeSegment(snapix::unifiedcache::Kind::Idx,
                                       static_cast<uint16_t>(markersKey));
                streamOffsetCache_.clear();
                LOG_INF(TAG, "[STREAM] idx stale/corrupt spine=%d (removed)", currentSpineIndex_);
              }
            }
          }

          // Look up best resume point in the cache: highest known
          // page <= currentSectionPage_.  If the target page itself
          // is cached, that's a 1-page render (no skip).  Otherwise
          // skip from the highest known prior page.
          snapix::smolport::MarkerizedRenderResume resume;
          if (!streamOffsetCache_.empty()) {
            for (auto it = streamOffsetCache_.rbegin(); it != streamOffsetCache_.rend(); ++it) {
              if (it->pageIndex <= static_cast<uint16_t>(currentSectionPage_)) {
                resume.startPage = it->pageIndex;
                resume.styleBits = it->styleBits;
                // v2.0.140 fix — also propagate the absolute byte
                // offset to the paginator's PageCountingObserver so
                // boundary callbacks captured during this resume
                // render report ABSOLUTE source positions, not
                // relative-to-seek-start.  Without this, the
                // streamOffsetCache_ accumulates relative offsets for
                // every page past the cold-walked range, and the next
                // page-turn seeks to a way-too-early position →
                // "first 3 lines change between pages" on-device.
                resume.byteOffset = it->byteOffset;
                // v3.5.2 — restore whether this resumed page begins a
                // paragraph, so the красная-строка first-line indent isn't
                // applied to a mid-sentence continuation page.
                resume.atParagraphStart = it->atParagraphStart;
                // v3.10.5 — restore block-level layout context so a page that
                // begins inside a <blockquote>/<li>/centered block wraps at the
                // indented width the .idx was built with (else the resumed break
                // diverges from it->byteOffset → a word duplicated/eaten here).
                resume.indentDepth = it->indentDepth;
                resume.centered = it->centered;
                // Seek to the captured GLOBAL offset; ChunkedMarkersReader
                // maps it to the containing chunk + local position and reads
                // forward (spanning chunk boundaries) from there.
                markersReader.seekGlobal(it->byteOffset);
                break;
              }
            }
          }

          // v3.9.0 — read through ChunkedMarkersReader: it bounds reads to the
          // logical stream (all chunks) and spans chunk boundaries internally,
          // so the paginator never runs past the markers into neighbouring data.
          auto readFn = [&markersReader](uint8_t* buf, size_t bufSize) -> int {
            return markersReader.read(buf, bufSize);
          };

          // Capture every new boundary into the cache so subsequent
          // renders can reuse them.  We append in order, but a render
          // may rediscover a page we already have cached (shouldn't
          // happen with proper resume, but defensive).
          auto onBoundary = [this](const snapix::smolport::PageBoundarySnapshot& s) {
            if (!streamOffsetCache_.empty() &&
                streamOffsetCache_.back().pageIndex >= s.pageIndex) {
              return;  // already cached this page or earlier — skip
            }
            streamOffsetCache_.push_back(s);
          };

          snapix::smolport::MarkerizedRenderStats stats{};
          const auto status = snapix::smolport::renderMarkerizedPage(
              paginator, readFn, chunkBuf, sizeof(chunkBuf),
              static_cast<uint16_t>(currentSectionPage_), {}, &stats, resume, onBoundary);
          // markersReader / chunkProvider close their file handle(s) on scope exit.

        if (status == snapix::smolport::MarkerizedRenderResult::Success) {
          LOG_INF(TAG,
                  "[STREAM] markerized render done type=%s spine=%d page=%d skipped=%u bytes=%u resume=%u cached=%u",
                  streamContentTypeName(contentType), currentSpineIndex_, currentSectionPage_,
                  static_cast<unsigned>(stats.pagesAdvancedThrough),
                  static_cast<unsigned>(stats.bytesConsumed),
                  static_cast<unsigned>(resume.startPage),
                  static_cast<unsigned>(streamOffsetCache_.size()));
          streamedOK = true;
        } else {
          LOG_INF(TAG,
                  "[STREAM] markerized render fallback type=%s spine=%d page=%d status=%u (legacy path)",
                  streamContentTypeName(contentType), currentSpineIndex_, currentSectionPage_,
                  static_cast<unsigned>(status));
        }
      }
    }
  }
  if (streamedOK) return;
#endif

  page.render(renderer_, fontId, marginLeft, marginTop, theme.primaryTextBlack);
}

void ReaderState::renderStatusBar(Core& core, int marginRight, int marginBottom, int marginLeft, int totalPages,
                                  bool isPartial) {
  if (core.settings.statusBar == Settings::StatusNone) {
    return;
  }

  const Theme& theme = THEME_MANAGER.current();

  // Build status bar data
  ui::ReaderStatusBarData data{};
  data.mode = core.settings.statusBar;
  data.title = core.content.metadata().title;

  // Resolve chapter title if in Chapter mode (cached to avoid SD I/O on every render)
  if (data.mode == Settings::StatusChapter && core.content.tocCount() > 0) {
    if (currentSpineIndex_ != cachedChapterSpine_ || currentSectionPage_ != cachedChapterPage_) {
      cachedChapterTitle_[0] = '\0';
      int tocIndex = findCurrentTocEntry(core);
      if (tocIndex >= 0) {
        auto result = core.content.getTocEntry(tocIndex);
        if (result.ok()) {
          strncpy(cachedChapterTitle_, result.value.title, sizeof(cachedChapterTitle_) - 1);
          cachedChapterTitle_[sizeof(cachedChapterTitle_) - 1] = '\0';
        }
      }
      cachedChapterSpine_ = currentSpineIndex_;
      cachedChapterPage_ = currentSectionPage_;
    }
    if (cachedChapterTitle_[0] != '\0') {
      data.title = cachedChapterTitle_;
    }
  }

  // Battery
  const uint16_t millivolts = batteryMonitor.readMillivolts();
  data.batteryPercent = (millivolts < 100) ? -1 : BatteryMonitor::percentageFromMillivolts(millivolts);

  const GlobalPageMetrics metrics = resolveGlobalPageMetrics(core, totalPages, isPartial);
  data.currentPage = metrics.currentPage;
  data.totalPages = metrics.totalPages;
  data.isPartial = data.totalPages <= 0 || !metrics.totalIsExact;

  ui::readerStatusBar(renderer_, theme, marginLeft, marginRight, marginBottom, data);
}

void ReaderState::renderXtcPage(Core& core) {
  auto* provider = core.content.asXtc();
  if (!provider) {
    return;
  }

  const Theme& theme = THEME_MANAGER.current();

  // v2.0.202 — explicit clearScreen (renderCurrentPage no longer
  // eagerly clears; only render-bearing branches do).  XTC takes
  // over the full screen via xtcRenderer_.render, so a clean
  // background is required.
  renderer_.clearScreen(theme.backgroundColor);

  auto result = xtcRenderer_.render(provider->getParser(), currentPage_, [this, &core]() { displayWithRefresh(core); });

  switch (result) {
    case XtcPageRenderer::RenderResult::Success:
      if (provider->getParser().getBitDepth() == 2) {
        pagesUntilFullRefresh_ = 1;
      }
      break;
    case XtcPageRenderer::RenderResult::EndOfBook:
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "End of book");
      break;
    case XtcPageRenderer::RenderResult::InvalidDimensions:
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "Invalid file");
      break;
    case XtcPageRenderer::RenderResult::AllocationFailed:
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "Memory error");
      break;
    case XtcPageRenderer::RenderResult::PageLoadFailed:
      ui::centeredMessage(renderer_, theme, theme.uiFontId, "Page load error");
      break;
  }
}

void ReaderState::displayWithRefresh(Core& core) {
  const bool turnOffScreen = core.settings.sunlightFadingFix != 0;
  const int pagesPerRefreshValue = core.settings.getPagesPerRefreshValue();

  // After an overlay banner (displayWindow), the RED RAM baseline is stale.
  // Drive-all refresh writes the inverted framebuffer to RED RAM so every
  // pixel is explicitly driven to its target state — no ghosting, no flash.
  if (forceCleanRefreshOnNext_) {
    forceCleanRefreshOnNext_ = false;
    LOG_DBG(TAG, "Refresh policy: frame=text mode=drive-all reason=overlay-cleanup");
    renderer_.displayBufferDriveAll(turnOffScreen);
    pagesUntilFullRefresh_ = pagesPerRefreshValue > 0 ? static_cast<uint8_t>(pagesPerRefreshValue) : 0;
    return;
  }

  if (core.wokeFromSleep) {
    LOG_DBG(TAG, "Refresh policy: frame=text mode=fast reason=wake");
    renderer_.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);
    core.wokeFromSleep = false;
    pagesUntilFullRefresh_ = pagesPerRefreshValue > 0 ? static_cast<uint8_t>(pagesPerRefreshValue - 1) : 0;
  } else if (pagesPerRefreshValue == 0) {
    LOG_DBG(TAG, "Refresh policy: frame=text mode=fast reason=cadence-disabled");
    renderer_.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);
    pagesUntilFullRefresh_ = 0;
  } else if (pagesUntilFullRefresh_ <= 1) {
    LOG_DBG(TAG, "Refresh policy: frame=text mode=half reason=cadence");
    renderer_.displayBuffer(EInkDisplay::HALF_REFRESH, turnOffScreen);
    pagesUntilFullRefresh_ = pagesPerRefreshValue;
  } else {
    LOG_DBG(TAG, "Refresh policy: frame=text mode=fast reason=cadence");
    renderer_.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);
    pagesUntilFullRefresh_--;
  }
}

void ReaderState::renderLoadingStatusMessage(Core& core) {
  if (contentLoaded_ && core.content.metadata().type == ContentType::Fb2 && core.content.tocCount() == 0) {
    renderCenteredStatusMessage(core, "No chapter list found", THEME_MANAGER.current().uiFontId,
                                "Loading may take a few minutes");
    return;
  }
  renderCenteredStatusMessage(core, "Loading...");
}

void ReaderState::renderCenteredStatusMessage(Core& core, const char* message, int fontIdOverride,
                                              const char* detail) {
  const Theme& theme = THEME_MANAGER.current();
  const bool turnOffScreen = core.settings.sunlightFadingFix != 0;
  const int fontId = fontIdOverride ? fontIdOverride : core.settings.getReaderFontId(theme);

  // v2.0.205 — single STATIC "Loading..." banner centred over whatever is
  // already on screen.  The animated spinner (v2.0.196-204) was removed: the
  // ESP32-C3 is too slow to repaint it without starving the FB2/EPUB parse
  // worker that shares the SPI bus, and the per-tick refresh dominated cold
  // loads.  A static banner costs exactly one drive-all and then leaves the
  // bus entirely to the worker.  Layout:
  //
  //   +---- bannerW ----+
  //   |                 |
  //   |    Loading...   |
  //   |                 |
  //   +-----------------+
  constexpr int padH = 24;  // horizontal padding inside banner
  constexpr int padV = 16;  // vertical padding inside banner

  const bool hasDetail = detail != nullptr && detail[0] != '\0';
  const int textWidth  = renderer_.getTextWidth(fontId, message, EpdFontFamily::BOLD);
  const int detailWidth =
      hasDetail ? renderer_.getTextWidth(fontId, detail, EpdFontFamily::REGULAR) : 0;
  const int lineHeight = renderer_.getLineHeight(fontId);
  const int lineGap    = hasDetail ? std::max(4, lineHeight / 3) : 0;
  const int bannerW    = std::max(textWidth, detailWidth) + padH * 2;
  const int bannerH    = lineHeight + (hasDetail ? lineGap + lineHeight : 0) + padV * 2;
  const int bannerX    = (renderer_.getScreenWidth() - bannerW) / 2;
  const int bannerY    = (renderer_.getScreenHeight() - bannerH) / 2;
  const int textX      = bannerX + (bannerW - textWidth) / 2;
  const int textY      = bannerY + padV;
  const int detailX    = bannerX + (bannerW - detailWidth) / 2;
  const int detailY    = textY + lineHeight + lineGap;

  // Draw overlay banner on top of existing framebuffer content — no clearScreen,
  // so the previous page / file-list / chapters menu stays visible behind the
  // banner (the v2.0.202 over-content fix: renderCurrentPage no longer wipes
  // the framebuffer before this banner is drawn).
  renderer_.fillRect(bannerX, bannerY, bannerW, bannerH, !theme.primaryTextBlack);
  renderer_.drawText(fontId, textX, textY, message, theme.primaryTextBlack, EpdFontFamily::BOLD);
  if (hasDetail) {
    renderer_.drawText(fontId, detailX, detailY, detail, theme.primaryTextBlack, EpdFontFamily::REGULAR);
  }
  renderer_.drawRect(bannerX + 3, bannerY + 3, bannerW - 6, bannerH - 6, theme.primaryTextBlack);

  // Use drive-all refresh for the full screen so every pixel — including the
  // file-list / page behind the banner — is actively driven to its correct
  // state.  A simple displayWindow (partial BW write + full-panel fast scan)
  // causes cross-refresh ghosting: the scan disturbs e-ink particles that were
  // recently driven by the previous state's fast refresh but haven't fully
  // settled, making the prior frame's cursor / content visibly reappear.
  // Drive-all eliminates this by making every pixel appear "changed" (inverted
  // RED RAM), so the SSD1677 applies a full driving pulse everywhere.
  LOG_DBG(TAG, "Refresh policy: frame=overlay mode=drive-all reason=banner (%dx%d at %d,%d)",
          bannerW, bannerH, bannerX, bannerY);
  renderer_.displayBufferDriveAll(turnOffScreen);
  core.display.markDirty();

  // Drive-all already established a clean differential baseline, but the next
  // page render will replace the entire screen (banner + background → book
  // content).  Keep the flag so that transition also uses drive-all for a
  // crisp first page without ghosting from the banner.
  forceCleanRefreshOnNext_ = true;
}

ReaderState::Viewport ReaderState::getReaderViewport(bool showStatusBar) const {
  Viewport vp{};
  renderer_.getOrientedViewableTRBL(&vp.marginTop, &vp.marginRight, &vp.marginBottom, &vp.marginLeft);
  vp.marginLeft += horizontalPadding;
  vp.marginRight += horizontalPadding;
  const Theme& theme = THEME_MANAGER.current();
  // Optional per-theme reduction of the top margin so dense fonts can ride
  // closer to the bezel.  Floor at 2 px to keep glyph ascenders off the very
  // top edge regardless of the configured reduction.
  if (theme.readerMarginTopReduction > 0) {
    vp.marginTop = std::max(2, vp.marginTop - static_cast<int>(theme.readerMarginTopReduction));
  }
  if (showStatusBar) {
    int reserved;
    if (theme.statusBarReservedHeight > 0) {
      reserved = theme.statusBarReservedHeight;  // explicit per-theme override
    } else {
      // v3.5.0 — size the reserve to the status font's ACTUAL height instead
      // of the fixed 23 px.  The constant was set for a 28 px status font, but
      // the bundled themes use small ~10-14 px status fonts, so 23 px left an
      // empty band above the bar — most visible at tight line spacing.  Sizing
      // to the font reclaims that band (so a tightly-spaced page fits one more
      // line) and never clips a taller status font.  Geometry: readerStatusBar
      // draws at textY = screenHeight - marginBottom - 2 and extends DOWNWARD
      // by the status line height (+ any positive statusBarOffsetY), so the
      // reserve must cover that; +4 px breathing room, floor 10.
      const int sh = renderer_.getEffectiveLineHeight(theme.statusFontId);
      const int off = (theme.statusBarOffsetY > 0) ? static_cast<int>(theme.statusBarOffsetY) : 0;
      reserved = std::max(sh + 4 + off, 10);
    }
    vp.marginBottom += reserved;
  }
  vp.width = renderer_.getScreenWidth() - vp.marginLeft - vp.marginRight;
  vp.height = renderer_.getScreenHeight() - vp.marginTop - vp.marginBottom;
  return vp;
}

bool ReaderState::renderCoverPage(Core& core) {
  LOG_DBG(TAG, "Generating cover for reader...");
  if (core.content.metadata().type == ContentType::Epub) {
    const std::string existingCoverPath = core.content.getCoverPath();
    const bool coverCached = !existingCoverPath.empty() && SdMan.exists(existingCoverPath.c_str());
    if (!coverCached) {
      const reader::HeapState heap = reader::readHeapState();
      if (reader::isHeapCritical(heap)) {
        LOG_INF(TAG, "Skipping uncached EPUB cover due to tight heap free=%u largest=%u",
                static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlock));
        return false;
      }
    }
  }

  std::string coverPath = core.content.generateCover(true);  // Always 1-bit in reader (saves ~48KB grayscale buffer)

  // v2.0.101 (architectural fix — post-cover "no-return" point):
  // Cover-gen via JPEGDEC pins ~25 KB (shared decoder instance) + ~7-32 KB
  // (decode arena) mid-heap for the rest of the session.  The next chapter
  // parser then starts with `largest_free` artificially shrunk to ~25 KB
  // instead of the ~70 KB it actually has — and a handful of ParsedText
  // allocations later (each ~1.8 KB contiguous from words.reserve(64))
  // collapse below the MIN_LAYOUT_FREE_HEAP=10240 watermark, aborting
  // every retry forever.  The "no-return" point.
  //
  // Releasing the JPEGDEC instance + arena here is cheap (one-line free,
  // ~100 µs) and the next image decode pays one re-allocation (~10 ms),
  // which is in the noise compared to the 50-200 ms per-image decode
  // total.  releaseAllPersistent() is a no-op when SNAPIX_SMOL_JPEG=1
  // (v3_alpha) because the SmolJpeg path never instantiates JPEGDEC at
  // all — costs nothing in either env, helps default env enormously.
  JpegToBmpConverter::releaseAllPersistent();

  if (coverPath.empty()) {
    LOG_DBG(TAG, "No cover available, skipping cover page");
    return false;
  }

  LOG_DBG(TAG, "Rendering cover page from: %s", coverPath.c_str());
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  int pagesUntilRefresh = pagesUntilFullRefresh_;
  const bool turnOffScreen = core.settings.sunlightFadingFix != 0;

  bool rendered = CoverHelpers::renderCoverFromBmp(renderer_, coverPath, vp.marginTop, vp.marginRight, vp.marginBottom,
                                                   vp.marginLeft, pagesUntilRefresh,
                                                   core.settings.getPagesPerRefreshValue(), turnOffScreen);

  // После полноэкранной обложки следующая текстовая страница должна пройти через
  // обычную более "тяжёлую" ступень cadence, но без отдельного промежуточного кадра.
  pagesUntilFullRefresh_ = 1;
  return rendered;
}

bool ReaderState::isWorkerRunning() const { return asyncJobs_.isJobRunning(); }

BackgroundTask::State ReaderState::workerState() const { return asyncJobs_.workerState(); }

void ReaderState::requestWorkerCancel() { asyncJobs_.requestCancelCurrentJob(); }

bool ReaderState::waitWorkerIdle(const uint32_t maxWaitMs) { return asyncJobs_.waitUntilIdle(maxWaitMs); }

void ReaderState::startBackgroundCaching(Core& core, const char* trigger) {
  if (!contentLoaded_) {
    return;
  }

  // Refuse all background cache work when heap is critically low.
  // Cold extends allocate a full ContentParser; if that pushes free heap
  // below ~10 KB the SdFat driver's internal state can become corrupt,
  // causing cascading SD-card access failures (files vanishing, directories
  // unreadable) that persist until reboot.
  //
  // v2.0.52 heap-pressure relief: ImageRenderCache can pin 30+ KB of
  // contiguous heap that fragments the largest-free-block down past the
  // worker's 10 KB threshold.  Once that happens, the worker can never
  // run, the cache can never shrink (no eviction trigger), and image
  // decodes stall forever (the v2.0.51 deadlock seen at
  // free=17864/largest=8692 in the user's log).  Before giving up on the
  // BG worker, drop the cache and re-check — the cache is purely an
  // optimisation and rebuilding it lazily on next render is far cheaper
  // than letting the decode pipeline deadlock.
  {
    auto heap = reader::readHeapState();
    if (reader::isHeapCritical(heap)) {
      const size_t cacheBytes = renderer_.imageCache().totalBytes();
      if (cacheBytes > 0) {
        LOG_INF(TAG, "[ASYNC] heap critical (free=%u largest=%u) — dropping image cache (%u bytes)",
                static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlock),
                static_cast<unsigned>(cacheBytes));
        renderer_.imageCache().clear();
        heap = reader::readHeapState();
      }
      // v2.0.194 — emergency hygiene escalation.  v2.0.183/190 added
      // proactive clears at book exit; v2.0.194 also fires the same
      // clears REACTIVELY here when the BG worker is about to skip
      // because heap is critical and dropping the image cache alone
      // wasn't enough.  Same caches as the exit-time hygiene; same
      // re-warm cost (~µs malloc/realloc per cache on next use).
      //
      // Triggers only when:
      //   1. heap was critical AT entry
      //   2. dropping imageCache didn't recover enough
      // — i.e. exactly the cases v2.0.193 logs showed gating the
      // background worker indefinitely (Free 35K, largest 9K, stuck
      // for minutes per the user's hardware repro).
      if (reader::isHeapCritical(heap)) {
        const auto heapBefore = heap;
        FONT_MANAGER.clearStreamingBitmapCaches();
        renderer_.clearWidthCache();
        renderer_.freeBitmapRowBuffers();
        heap = reader::readHeapState();
        LOG_INF(TAG,
                "[ASYNC] heap-critical escalation: freed font bitmaps + "
                "width cache + bitmap rows (free=%u->%u largest=%u->%u)",
                static_cast<unsigned>(heapBefore.freeBytes), static_cast<unsigned>(heap.freeBytes),
                static_cast<unsigned>(heapBefore.largestBlock), static_cast<unsigned>(heap.largestBlock));
      }
      if (reader::isHeapCritical(heap)) {
        LOG_DBG(TAG, "[ASYNC] skip background cache: heap critical (free=%u largest=%u)",
                static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlock));
        return;
      }
    }
  }

  if (pendingTocJumpActive_ || pendingEpubPageLoadActive_ || tocMode_ || bookmarkMode_ || menuMode_) {
    LOG_INF(TAG,
            "[ASYNC] skip background cache during pending/overlay state (toc=%u bookmark=%u menu=%u tocJump=%u pageLoad=%u)",
            static_cast<unsigned>(tocMode_), static_cast<unsigned>(bookmarkMode_), static_cast<unsigned>(menuMode_),
            static_cast<unsigned>(pendingTocJumpActive_), static_cast<unsigned>(pendingEpubPageLoadActive_));
    return;
  }

  const BackgroundCachePlan plan = planBackgroundCacheWork(core);
  if (!plan.shouldStart) {
    return;
  }

  if (trigger && strcmp(trigger, "post-render") == 0 && plan.reason == BackgroundCacheWakeReason::FarPrefetchReady) {
    LOG_DBG(TAG, "[CACHE] background cache skip trigger=%s reason=far-prefetch-deferred activeSpine=%d candidate=%d",
            trigger, plan.activeSpine, plan.candidateSpine);
    return;
  }

  if (core.content.metadata().type == ContentType::Xtc) {
    if (!thumbnailDone_) {
      core.content.generateCover(true);
      core.content.generateThumbnail();
      thumbnailDone_ = true;
    }
    return;
  }

  if (isWorkerRunning()) {
    LOG_DBG(TAG, "[ASYNC] background cache request ignored because worker is already busy");
    return;
  }

  reader::ReaderAsyncJobsController::BackgroundCacheRequest request;
  request.position.currentPage = currentPage_;
  request.position.currentSpineIndex = currentSpineIndex_;
  request.position.currentSectionPage = currentSectionPage_;
  request.position.lastRenderedSpineIndex = lastRenderedSpineIndex_;
  request.position.lastRenderedSectionPage = lastRenderedSectionPage_;
  request.position.hasCover = hasCover_;
  request.position.textStartIndex = textStartIndex_;
  request.plan = plan;
  request.showStatusBar = core.settings.statusBar != 0;
  if (trigger) {
    strlcpy(request.trigger, trigger, sizeof(request.trigger));
  }

  lastIdleBackgroundKickMs_ = millis();
  if (!asyncJobs_.queueBackgroundCache(request)) {
    LOG_ERR(TAG, "[ASYNC] failed to queue background cache trigger=%s reason=%s", trigger ? trigger : "auto",
            backgroundCacheWakeReasonToString(plan.reason));
  }
}

bool ReaderState::requestBackgroundCachingPause(const char* siteTag) {
  // v2.0.110 (audit fix #1): non-blocking-ish cancel for render path.
  // Worker yields cooperatively on shouldAbort, typically within ~100 ms.
  // We wait up to `kInteractiveCacheCancelTimeoutMs` (500 ms) for that
  // honest yield — long enough for normal cancellation, short enough that
  // input polling resumes in the human-perceptible "instant" window.
  // No ESP.restart() on timeout: render proceeds against on-disk cache
  // (the worker can't corrupt that — writes use .rebuild sidecar with
  // atomic rename, and the in-memory pageCache_ access is read-only on
  // the render path).
  if (!isWorkerRunning()) {
    return true;
  }
  requestWorkerCancel();
  if (!waitWorkerIdle(kInteractiveCacheCancelTimeoutMs)) {
    LOG_INF(TAG, "[ASYNC] interactive-cancel timeout site=%s — proceeding (worker still draining)",
            siteTag ? siteTag : "?");
    return false;
  }
  return true;
}

bool ReaderState::stopBackgroundCaching() {
  if (!isWorkerRunning()) {
    return true;
  }

  requestWorkerCancel();
  if (!waitWorkerIdle(kCacheTaskStopTimeoutMs)) {
    // The worker is genuinely stuck — likely a deadlock in SharedBusLock or
    // a tight loop in picojpeg / parser that didn't honour shouldAbort.  Log
    // a forensic snapshot before the nuclear restart so the next session can
    // tell us *why* this fired (free heap shrinking? stuck on a particular
    // book?).  A clean reboot is safer than tearing down state with a live
    // task still touching it.
    const reader::HeapState heap = reader::readHeapState();
    LOG_ERR(TAG, "[ASYNC] worker did not stop within %d ms timeout", kCacheTaskStopTimeoutMs);
    LOG_ERR(TAG, "[ASYNC] forensic snapshot: free=%u largest=%u spine=%d page=%d path=%s",
            static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlock),
            currentSpineIndex_, currentSectionPage_, contentPath_);
    LOG_ERR(TAG, "Restarting to avoid unsafe cache/parser teardown after stop timeout");
    vTaskDelay(50 / portTICK_PERIOD_MS);
    ESP.restart();
    return false;
  }
  return true;
}

}  // namespace snapix
