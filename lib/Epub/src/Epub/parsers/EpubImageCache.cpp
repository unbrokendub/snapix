#include "EpubImageCache.h"

// v2.0.164 — this TU was 2400 LOC before the R-series trilogy
// (v2.0.161-163) deleted the legacy parser pipeline.  All that's left is
// the image-cache machinery `ReaderState::renderPageContents` calls during
// streaming render.  Removed includes: <Page.h>, <Utf8.h>, <expat.h>,
// <freertos/*>, <EpubRenderWorkspace.h>, "../htmlEntities.h" — none of
// them are referenced by the surviving methods.
#include <FS.h>
#include <LittleFS.h>
#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <ImageConverter.h>
#include <Logging.h>
#include <SDCardManager.h>
#include <SharedSpiLock.h>
#include <esp_heap_caps.h>

#include "../PendingImageDecode.h"  // v2.0.83: async image-decode queue

#include <algorithm>
#include <unordered_set>
#define TAG "HTML_PARSER"

// v2.0.164 — destructor used to call `cleanupParser()` (now deleted with
// the parse loop).  Defaulted; member dtors free heap-owned state.
EpubImageCache::~EpubImageCache() = default;

namespace {

enum class ImageInterruptReason : uint8_t {
  None,
  StopRequested,
  Timeout,
  LowMemory,
};

const char* readItemStatusToString(const EpubImageCache::ReadItemStatus status) {
  switch (status) {
    case EpubImageCache::ReadItemStatus::Success:
      return "success";
    case EpubImageCache::ReadItemStatus::Aborted:
      return "aborted";
    case EpubImageCache::ReadItemStatus::NotFound:
      return "not-found";
    case EpubImageCache::ReadItemStatus::ArchiveError:
      return "archive-error";
    case EpubImageCache::ReadItemStatus::IoError:
      return "io-error";
    case EpubImageCache::ReadItemStatus::WriteError:
      return "write-error";
  }
  return "unknown";
}

const char* cachedImageStatusToString(const EpubImageCache::CachedImageStatus status) {
  switch (status) {
    case EpubImageCache::CachedImageStatus::Success:
      return "success";
    case EpubImageCache::CachedImageStatus::RetryableInterruption:
      return "retryable-interruption";
    case EpubImageCache::CachedImageStatus::TerminalFailure:
      return "terminal-failure";
  }
  return "unknown";
}

const char* imageFailureClassToString(const EpubImageCache::ImageFailureClass failureClass) {
  switch (failureClass) {
    case EpubImageCache::ImageFailureClass::None:
      return "none";
    case EpubImageCache::ImageFailureClass::AbortRequested:
      return "abort-requested";
    case EpubImageCache::ImageFailureClass::Timeout:
      return "timeout";
    case EpubImageCache::ImageFailureClass::LowMemory:
      return "low-memory";
    case EpubImageCache::ImageFailureClass::DataUri:
      return "data-uri";
    case EpubImageCache::ImageFailureClass::UnsupportedFormat:
      return "unsupported-format";
    case EpubImageCache::ImageFailureClass::CacheHit:
      return "cache-hit";
    case EpubImageCache::ImageFailureClass::CachedOpenFailed:
      return "cached-open-failed";
    case EpubImageCache::ImageFailureClass::CachedBmpInvalid:
      return "cached-bmp-invalid";
    case EpubImageCache::ImageFailureClass::TempOpenFailed:
      return "temp-open-failed";
    case EpubImageCache::ImageFailureClass::ExtractAborted:
      return "extract-aborted";
    case EpubImageCache::ImageFailureClass::ExtractFailed:
      return "extract-failed";
    case EpubImageCache::ImageFailureClass::ConvertAborted:
      return "convert-aborted";
    case EpubImageCache::ImageFailureClass::ConvertFailed:
      return "convert-failed";
    case EpubImageCache::ImageFailureClass::GeneratedBmpInvalid:
      return "generated-bmp-invalid";
    case EpubImageCache::ImageFailureClass::MissingSrc:
      return "missing-src";
    case EpubImageCache::ImageFailureClass::ReadItemUnavailable:
      return "read-item-unavailable";
    case EpubImageCache::ImageFailureClass::ImageCacheDisabled:
      return "image-cache-disabled";
  }
  return "unknown";
}

}  // namespace

bool EpubImageCache::validateCachedBmp(const std::string& cachedBmpPath, uint16_t& width, uint16_t& height,
                                              std::string& failureReason) {
  // v2.0.73: cached BMPs live on LittleFS now (image cache moved with the
  // rest of the EPUB cache).  No SharedBusLock needed for LittleFS reads.
  File bmpFile = LittleFS.open(cachedBmpPath.c_str(), "r");
  if (!bmpFile) {
    failureReason = "cached-open-failed";
    return false;
  }

  Bitmap bitmap(bmpFile, false);
  const BmpReaderError err = bitmap.parseHeaders();
  if (err != BmpReaderError::Ok) {
    failureReason = std::string("cached-bmp-parse-failed:") + Bitmap::errorToString(err);
    bmpFile.close();
    return false;
  }

  width = static_cast<uint16_t>(bitmap.getWidth());
  height = static_cast<uint16_t>(bitmap.getHeight());
  bmpFile.close();
  failureReason.clear();
  return true;
}

// v2.0.148 — public lazy image-cache entry point.  Delegates to the
// private cacheImage; converts the rich CachedImageResult into the
// simpler `(bool, path, w, h)` shape the streaming resolver wants.
// See header for the contract.
bool EpubImageCache::cacheImageForStreaming(const std::string& src,
                                                     std::string& outBmpPath,
                                                     uint16_t& outWidth,
                                                     uint16_t& outHeight) {
  const CachedImageResult r = cacheImage(src);
  if (!r.success && !r.cacheHit) {
    // r.cachedBmpPath is set even on failure (it's the path we *would*
    // have written to); only treat as success if the BMP is actually
    // present on disk.  cacheHit covers the idempotent re-call case.
    return false;
  }
  outBmpPath = r.cachedBmpPath;
  outWidth = r.width;
  outHeight = r.height;
  return true;
}

EpubImageCache::CachedImageResult EpubImageCache::cacheImage(const std::string& src) {
  CachedImageResult result;
  result.resolvedPath = FsHelpers::normalisePath(chapterBasePath + src);
  result.status = CachedImageStatus::TerminalFailure;
  LOG_INF(TAG, "[CONTENT][IMAGE] start src=%s resolved=%s quick=%u", src.c_str(), result.resolvedPath.c_str(),
          static_cast<unsigned>(quickImageDecode_));
  const uint32_t imageStartedMs = millis();
  const uint32_t imageTimeoutMs =
      quickImageDecode_ ? MAX_QUICK_IMAGE_PROCESS_TIME_MS : MAX_FULL_IMAGE_PROCESS_TIME_MS;
  ImageInterruptReason lastSeenInterrupt = ImageInterruptReason::None;
  auto classifyImageInterrupt = [this, imageStartedMs, imageTimeoutMs,
                                 &lastSeenInterrupt]() -> ImageInterruptReason {
    if (stopRequested_ || cooperativeAbortRequested_) {
      return ImageInterruptReason::StopRequested;
    }
    // v2.0.80: check heap BEFORE the external abort callback.  ReaderAsync's
    // external abort fires at ~10 KB free (its own heap-critical threshold,
    // much looser than ours).  Pre-2.0.80 we'd reach the external check
    // first and classify as StopRequested — which means the parent
    // resolveAndCacheImage path didn't blacklist the image, leading to
    // identical retries on every cold-extend (see ConvertAborted not having
    // a blacklist branch).  Classifying as LowMemory when the heap is
    // actually low lets v2.0.78's blacklist logic kick in.
    const size_t freeHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (freeHeap < MIN_IMAGE_PROCESS_FREE_HEAP) {
      LOG_ERR(TAG, "Low memory during image processing (%zu bytes free)", freeHeap);
      lastSeenInterrupt = ImageInterruptReason::LowMemory;
      return ImageInterruptReason::LowMemory;
    }
    if (externalAbortCallback_ && externalAbortCallback_()) {
      // v2.0.80: external abort triggered but our own heap is still above
      // the parser threshold.  ReaderAsync's threshold is around 10 KB
      // (kHeapCriticalLargestBytes); if we passed the 4 KB parser check
      // above but the external callback fired, the proximate cause is
      // most likely heap pressure too.  Use freeHeap < 16 KB as a "this
      // is really low" heuristic to still classify as LowMemory and let
      // the v2.0.78 blacklist path kick in.  Otherwise fall back to
      // StopRequested for user-initiated aborts (page-flip, exit reader).
      if (freeHeap < 16 * 1024) {
        LOG_INF(TAG, "External abort with low heap (%zu bytes); treating as LowMemory", freeHeap);
        lastSeenInterrupt = ImageInterruptReason::LowMemory;
        return ImageInterruptReason::LowMemory;
      }
      return ImageInterruptReason::StopRequested;
    }
    if (millis() - imageStartedMs > imageTimeoutMs) {
      LOG_ERR(TAG, "Image processing timeout after %lu ms", static_cast<unsigned long>(imageTimeoutMs));
      return ImageInterruptReason::Timeout;
    }
    return ImageInterruptReason::None;
  };
  auto setRetryable = [&](const char* reason, const ImageFailureClass failureClass) {
    result.failureReason = reason;
    result.failureClass = failureClass;
    result.retryable = true;
    result.status = CachedImageStatus::RetryableInterruption;
    result.success = false;
  };
  auto setTerminal = [&](const char* reason, const ImageFailureClass failureClass) {
    result.failureReason = reason;
    result.failureClass = failureClass;
    result.retryable = false;
    result.status = CachedImageStatus::TerminalFailure;
    result.success = false;
  };
  auto interruptToFailureClass = [](const ImageInterruptReason reason,
                                    const ImageFailureClass stopClass) -> ImageFailureClass {
    switch (reason) {
      case ImageInterruptReason::StopRequested:
        return stopClass;
      case ImageInterruptReason::Timeout:
        return ImageFailureClass::Timeout;
      case ImageInterruptReason::LowMemory:
        return ImageFailureClass::LowMemory;
      case ImageInterruptReason::None:
        break;
    }
    return stopClass;
  };
  auto interruptToReason = [](const ImageInterruptReason reason, const char* stopReason) -> const char* {
    switch (reason) {
      case ImageInterruptReason::StopRequested:
        return stopReason;
      case ImageInterruptReason::Timeout:
        return "timeout";
      case ImageInterruptReason::LowMemory:
        return "low-memory";
      case ImageInterruptReason::None:
        break;
    }
    return stopReason;
  };
  auto shouldAbortImage = [&]() -> bool {
    return classifyImageInterrupt() != ImageInterruptReason::None;
  };

  // Check abort before starting image processing
  if (const ImageInterruptReason interrupt = classifyImageInterrupt(); interrupt != ImageInterruptReason::None) {
    LOG_DBG(TAG, "Retryable image interruption before start");
    setRetryable(interruptToReason(interrupt, "abort-before-start"),
                 interruptToFailureClass(interrupt, ImageFailureClass::AbortRequested));
    return result;
  }

  // Skip data URIs - embedded base64 images can't be extracted and waste memory
  if (src.length() >= 5 && strncasecmp(src.c_str(), "data:", 5) == 0) {
    LOG_DBG(TAG, "Skipping embedded data URI image");
    setTerminal("data-uri", ImageFailureClass::DataUri);
    return result;
  }

  // Generate cache filename from hash
  size_t srcHash = std::hash<std::string>{}(result.resolvedPath);
  result.cachedBmpPath = imageCachePath + "/" + std::to_string(srcHash) + ".bmp";

  // Session blacklist: image already failed with timeout/abort this boot.
  // Skip immediately to break the infinite-retry loop in cold-extend.
  auto& failedHashes = sessionFailedImageHashes();
  if (failedHashes.count(srcHash)) {
    LOG_INF(TAG, "[CONTENT][IMAGE] session-blacklisted src=%s resolved=%s (skipped)", src.c_str(),
            result.resolvedPath.c_str());
    setTerminal("session-blacklisted", ImageFailureClass::ConvertFailed);
    return result;
  }

  std::string failedMarker = imageCachePath + "/" + std::to_string(srcHash) + ".failed";
  uint16_t cachedWidth = 0;
  uint16_t cachedHeight = 0;
  std::string cacheFailure;
  std::string tempPath;

  // ── Phase 1: cache check + extraction ──────────────────────────────
  // v2.0.73: image cache lives on LittleFS now (separate SPI bus, no
  // SharedBusLock contention with display).  The readItemFn callback that
  // extracts from the EPUB ZIP DOES still touch SD — but that callback
  // owns its own locking internally.
  auto writeFailedMarker = [&]() {
    File marker = LittleFS.open(failedMarker.c_str(), "w");
    if (marker) marker.close();
  };
  {
    // Check if already cached and validate the cached BMP before trusting it.
    if (LittleFS.exists(result.cachedBmpPath.c_str())) {
      result.cacheHit = true;
      if (validateCachedBmp(result.cachedBmpPath, cachedWidth, cachedHeight, cacheFailure)) {
        consecutiveImageFailures_ = 0;
        result.width = cachedWidth;
        result.height = cachedHeight;
        result.success = true;
        result.retryable = false;
        result.status = CachedImageStatus::Success;
        result.failureClass = ImageFailureClass::CacheHit;
        result.failureReason = "cache-hit";
        LOG_INF(TAG, "[CONTENT][IMAGE] cache hit src=%s resolved=%s cached=%s size=%ux%u", src.c_str(),
                result.resolvedPath.c_str(), result.cachedBmpPath.c_str(), static_cast<unsigned>(result.width),
                static_cast<unsigned>(result.height));
        return result;
      }

      LOG_INF(TAG, "[CONTENT][IMAGE] cache invalid src=%s resolved=%s cached=%s reason=%s action=rebuild", src.c_str(),
              result.resolvedPath.c_str(), result.cachedBmpPath.c_str(), cacheFailure.c_str());
      LittleFS.remove(result.cachedBmpPath.c_str());
      LittleFS.remove(failedMarker.c_str());
    }

    // Failed markers are only trusted for extraction/format failures.  Conversion
    // can still succeed later in quick mode, so allow supported images to retry.
    if (LittleFS.exists(failedMarker.c_str())) {
      if (!ImageConverterFactory::isSupported(src)) {
        consecutiveImageFailures_++;
        setTerminal("failed-marker-unsupported-format", ImageFailureClass::UnsupportedFormat);
        return result;
      }
      LittleFS.remove(failedMarker.c_str());
    }

    if (!ImageConverterFactory::isSupported(src)) {
      LOG_DBG(TAG, "Unsupported image format: %s", src.c_str());
      writeFailedMarker();
      consecutiveImageFailures_++;
      setTerminal("unsupported-format", ImageFailureClass::UnsupportedFormat);
      return result;
    }

    // Extract image to LittleFS temp file (hash in name for uniqueness).
    const std::string tempExt = FsHelpers::isPngFile(src) ? ".png" : ".jpg";
    tempPath = imageCachePath + "/.tmp_" + std::to_string(srcHash) + tempExt;

    // Foreground streaming mode: enqueue BOTH ZIP extraction and conversion.
    // Previously quick mode still inflated every encountered image before it
    // returned.  On a saved page inside a 213-page image-heavy chapter that
    // meant 14 serial extractions and four 15-second timeouts before any text
    // appeared.  The ReaderAsync worker now prepares only images referenced by
    // the visible render and repaints when their BMPs are ready.
    if (quickImageDecode_) {
      if (snapix::pendingImage::isPendingOrActive(result.cachedBmpPath)) {
        (void)snapix::pendingImage::promote(result.cachedBmpPath);
        result.success = true;
        result.retryable = false;
        result.status = CachedImageStatus::Success;
        result.failureClass = ImageFailureClass::None;
        result.failureReason = "async-pending";
        return result;
      }

      snapix::PendingImageDecode item;
      item.tempJpegPath = tempPath;
      item.targetBmpPath = result.cachedBmpPath;
      item.maxWidth = static_cast<uint16_t>(config.viewportWidth);
      item.maxHeight = static_cast<uint16_t>(config.viewportHeight);
      item.srcHash = static_cast<uint32_t>(srcHash);
      item.quickMode = true;
      item.logTag = "EHP";

      const auto deferredReadItem = readItemFn;
      const std::string deferredHref = result.resolvedPath;
      item.prepareInput =
          [deferredReadItem, deferredHref](
              const std::string& deferredTempPath,
              const std::function<bool()>& abort) -> bool {
        File tempFile = LittleFS.open(deferredTempPath.c_str(), "w");
        if (!tempFile) return false;
        static constexpr size_t kDeferredImageZipStreamChunk = 8192;
        const ReadItemStatus status =
            deferredReadItem(deferredHref, tempFile,
                             kDeferredImageZipStreamChunk, abort);
        tempFile.close();
        if (status != ReadItemStatus::Success) {
          LittleFS.remove(deferredTempPath.c_str());
          LOG_INF(TAG,
                  "[CONTENT][IMAGE] deferred extract result resolved=%s result=%s",
                  deferredHref.c_str(), readItemStatusToString(status));
          return false;
        }
        return true;
      };

      if (!snapix::pendingImage::enqueue(
              std::move(item),
              snapix::pendingImage::Priority::CurrentPage)) {
        setRetryable("async-queue-full", ImageFailureClass::TempOpenFailed);
        return result;
      }

      consecutiveImageFailures_ = 0;
      result.success = true;
      result.retryable = false;
      result.status = CachedImageStatus::Success;
      result.failureClass = ImageFailureClass::None;
      result.failureReason = "async-extract-deferred";
      LOG_INF(TAG,
              "[CONTENT][IMAGE] deferred extraction src=%s resolved=%s target=%s",
              src.c_str(), result.resolvedPath.c_str(),
              result.cachedBmpPath.c_str());
      return result;
    }

    File tempFile = LittleFS.open(tempPath.c_str(), "w");
    if (!tempFile) {
      LOG_ERR(TAG, "Failed to create temp file for image");
      setRetryable("temp-open-failed", ImageFailureClass::TempOpenFailed);
      return result;
    }

    // v2.0.86: 8 KB streaming chunk (was 1024).  Image entries are
    // typically 30-200 KB compressed in Calibre EPUBs (deflate-everything
    // setting); larger chunks cut the per-image SD-read overhead from
    // ~150-200 ms down to ~30-40 ms.  ZipFile briefly allocates 16 KB
    // (in+out buffers) — comfortably within the heap budget during a BG
    // image-decode pass since the JPEGDEC arena hasn't been allocated yet.
    static constexpr size_t kImageZipStreamChunk = 8192;
    const ReadItemStatus readStatus =
        readItemFn(result.resolvedPath, tempFile, kImageZipStreamChunk, shouldAbortImage);
    if (readStatus != ReadItemStatus::Success) {
      LOG_INF(TAG, "[CONTENT][IMAGE] extract result src=%s resolved=%s result=%s", src.c_str(),
              result.resolvedPath.c_str(), readItemStatusToString(readStatus));
      tempFile.close();
      LittleFS.remove(tempPath.c_str());
      if (readStatus == ReadItemStatus::Aborted) {
        // v2.0.78: distinguish user-cancel from automatic Timeout/LowMemory.
        // Timeout means the image is just too big for this hardware (typical:
        // a 400 KB ZIP-deflated JPEG in a Calibre-exported EPUB).  Pre-2.0.78
        // we returned `retryable` for any abort → caller retried 3x, each
        // attempt re-burned the timeout, page NEVER rendered.  Now we
        // session-blacklist the image hash AND mark terminal so the chapter
        // parser proceeds with a placeholder.  StopRequested (user button
        // preempt) stays retryable: user may navigate back and want the image
        // rendered properly.
        //
        // v2.0.91: LowMemory used to be blacklisted alongside Timeout, but
        // that proved too aggressive.  LowMemory is a TRANSIENT condition —
        // concurrent UI-render + BG cache parse can briefly fragment the
        // heap below the `MIN_FREE_HEAP=8 KB` abort threshold even when the
        // image itself is small enough to extract once the heap settles.
        // The user's image/38.jpg in Дорофеев "Путь джедая" hit this at
        // page-turn time: heap=7668 momentarily, image got blacklisted, then
        // 2 seconds later heap was 16 KB+ and the image would have
        // extracted fine — but it stayed a placeholder until reboot.
        // Now LowMemory falls through to the retryable path; the BG cache
        // cold-extend re-attempts on the next chapter visit when heap will
        // typically have recovered.
        const ImageInterruptReason effectiveInterrupt =
            (lastSeenInterrupt != ImageInterruptReason::None) ? lastSeenInterrupt
                                                              : classifyImageInterrupt();
        if (effectiveInterrupt == ImageInterruptReason::Timeout) {
          sessionFailedImageHashes().insert(srcHash);
          LOG_INF(TAG, "[CONTENT][IMAGE] extract blacklisted for session src=%s reason=%s",
                  result.resolvedPath.c_str(), interruptToReason(effectiveInterrupt, "extract-aborted"));
          consecutiveImageFailures_++;
          setTerminal(interruptToReason(effectiveInterrupt, "extract-aborted"),
                      interruptToFailureClass(effectiveInterrupt, ImageFailureClass::ExtractAborted));
          return result;
        }
        setRetryable(interruptToReason(effectiveInterrupt, "extract-aborted"),
                     interruptToFailureClass(effectiveInterrupt, ImageFailureClass::ExtractAborted));
        return result;
      }
      if (readStatus == ReadItemStatus::NotFound) {
        writeFailedMarker();
        consecutiveImageFailures_++;
        setTerminal("extract-not-found", ImageFailureClass::ExtractFailed);
        return result;
      }
      if (readStatus == ReadItemStatus::ArchiveError) {
        writeFailedMarker();
        consecutiveImageFailures_++;
        setTerminal("extract-archive-error", ImageFailureClass::ExtractFailed);
        return result;
      }
      setRetryable(readStatus == ReadItemStatus::WriteError ? "extract-write-error" : "extract-io-error",
                   ImageFailureClass::ExtractFailed);
      return result;
    }
    tempFile.close();
  }

  // ── Phase 2: conversion ────────────────────────────────────────────
  // v2.0.73: target lives on LittleFS (cachedBmpPath is /cache/epub_<hash>/
  // images/...) — set via outputOnLittleFs below.
  // v2.0.79: source tempPath also lives on LittleFS; ImageConverter now
  // auto-detects the input filesystem internally (no flag needed here).
  const int maxImageHeight = config.viewportHeight;
  const int maxImageWidth = static_cast<int>(config.viewportWidth);

  auto tryConvert = [&](int maxWidth, int maxHeight, bool quickMode) -> bool {
    ImageConvertConfig convertConfig;
    convertConfig.maxWidth = maxWidth;
    convertConfig.maxHeight = maxHeight;
    convertConfig.quickMode = quickMode;
    convertConfig.logTag = "EHP";
    convertConfig.shouldAbort = shouldAbortImage;
    convertConfig.outputOnLittleFs = true;
    // v2.0.89: 1-bit output so `convertJpegLittleFs` / `convertPngLittleFs`
    // route through the SmolJpeg / SmolPng one-pass decoders instead of
    // JPEGDEC / pngle.  The display panel is 1-bit anyway — inline EPUB
    // images get dithered to B/W on render either way — and the legacy
    // JPEGDEC path keeps OOM'ing on its ~25 KB shared workspace once the
    // heap has fragmented mid-chapter (image/2.jpg+ in the user's test
    // EPUB).  SmolJpeg's transient heap footprint is ~28 KB (vs ~50 KB
    // peak for JPEGDEC) and it never trips that workspace OOM.
    convertConfig.oneBit = true;
    return ImageConverterFactory::convertToBmp(tempPath, result.cachedBmpPath, convertConfig);
  };

  bool success = tryConvert(maxImageWidth, maxImageHeight, quickImageDecode_);
  if (!success && !shouldAbortImage() && !quickImageDecode_) {
    LOG_INF(TAG, "[CONTENT][IMAGE] retry quick src=%s", result.resolvedPath.c_str());
    LittleFS.remove(result.cachedBmpPath.c_str());
    success = tryConvert(maxImageWidth, maxImageHeight, true);
  }
  if (!success && !shouldAbortImage()) {
    const int fallbackWidth = std::max(64, (maxImageWidth * 3) / 4);
    const int fallbackHeight = std::max(64, (maxImageHeight * 3) / 4);
    if (fallbackWidth != maxImageWidth || fallbackHeight != maxImageHeight) {
      LOG_INF(TAG, "[CONTENT][IMAGE] retry reduced src=%s size=%dx%d", result.resolvedPath.c_str(), fallbackWidth,
              fallbackHeight);
      LittleFS.remove(result.cachedBmpPath.c_str());
      success = tryConvert(fallbackWidth, fallbackHeight, true);
    }
  }

  // ── Phase 3: post-conversion cleanup ───────────────────────────────
  {
    LittleFS.remove(tempPath.c_str());

    if (!success) {
      const ImageInterruptReason interrupt = classifyImageInterrupt();
      // Use lastSeenInterrupt when current check shows none — transient conditions
      // like low memory resolve after the converter frees its buffers.
      const ImageInterruptReason effectiveInterrupt =
          (interrupt != ImageInterruptReason::None) ? interrupt : lastSeenInterrupt;
      LOG_ERR(TAG, "[CONTENT][IMAGE] convert failed: %s interrupt=%s (last=%s)", result.resolvedPath.c_str(),
              interruptToReason(interrupt, "none"), interruptToReason(effectiveInterrupt, "none"));
      LittleFS.remove(result.cachedBmpPath.c_str());
      if (effectiveInterrupt != ImageInterruptReason::None) {
        // v2.0.91: Session-blacklist ONLY genuine Timeout (image really is
        // too big for this hardware — repeating the convert will just burn
        // the same wall-clock budget every cold-extend pass).  LowMemory is
        // transient (concurrent BG cache + UI-render contention) and the
        // image will typically extract fine on the next attempt once heap
        // settles — see v2.0.91 extract-phase change above for the user-
        // visible regression that motivated this.
        // User-aborted (StopRequested) is never blacklisted: user may
        // navigate back and want the image rendered properly.
        if (effectiveInterrupt == ImageInterruptReason::Timeout) {
          sessionFailedImageHashes().insert(srcHash);
          LOG_INF(TAG, "[CONTENT][IMAGE] blacklisted for session src=%s reason=%s", result.resolvedPath.c_str(),
                  interruptToReason(effectiveInterrupt, "convert-aborted"));
        }
        setRetryable(interruptToReason(effectiveInterrupt, "convert-aborted"),
                     interruptToFailureClass(effectiveInterrupt, ImageFailureClass::ConvertAborted));
        return result;
      }
      consecutiveImageFailures_++;
      setTerminal("convert-failed", ImageFailureClass::ConvertFailed);
      return result;
    }

    std::string generatedFailure;
    if (!validateCachedBmp(result.cachedBmpPath, cachedWidth, cachedHeight, generatedFailure)) {
      LOG_ERR(TAG, "[CONTENT][IMAGE] generated invalid bmp src=%s resolved=%s cached=%s reason=%s", src.c_str(),
              result.resolvedPath.c_str(), result.cachedBmpPath.c_str(), generatedFailure.c_str());
      LittleFS.remove(result.cachedBmpPath.c_str());
      consecutiveImageFailures_++;
      setTerminal("generated-bmp-invalid", ImageFailureClass::GeneratedBmpInvalid);
      return result;
    }

    LittleFS.remove(failedMarker.c_str());
  }

  consecutiveImageFailures_ = 0;
  result.width = cachedWidth;
  result.height = cachedHeight;
  result.success = true;
  result.retryable = false;
  result.status = CachedImageStatus::Success;
  result.failureClass = ImageFailureClass::None;
  result.failureReason = "generated";
  LOG_INF(TAG, "[CONTENT][IMAGE] done src=%s resolved=%s cached=%s elapsed=%lu size=%ux%u", src.c_str(),
          result.resolvedPath.c_str(), result.cachedBmpPath.c_str(),
          static_cast<unsigned long>(millis() - imageStartedMs), static_cast<unsigned>(result.width),
          static_cast<unsigned>(result.height));
  return result;
}
