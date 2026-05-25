#pragma once

// v2.0.164 — slimmed from 386 LOC of parser+image-cache to ~90 LOC of
// image-cache-only.  All members + methods related to the legacy expat-
// based chapter parser are gone (parseAndBuildPages, resumeParsing,
// initParser, parseLoop, makePages, XMLCALL handlers, SmolHtml shim,
// CSS parser ref, anchor map, suspend state, page-tree builders, etc.).
//
// What remains is the image-cache machinery `ReaderState::renderPage
// Contents` uses for inline EPUB image decode during streaming render
// (via `cacheImageForStreaming`).  The class name is unchanged so the
// one call site in ReaderState keeps compiling; the constructor still
// accepts the parser-era parameters (completePageFn, progressFn,
// cssParser) as ignored arguments so we don't have to touch ReaderState
// either.  A future cleanup can rename to `EpubImageCache` and drop
// the unused ctor params at the same time.

#include <FS.h>
#include <RenderConfig.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class GfxRenderer;
class Print;
// v2.0.171 — Page and CssParser fwd decls dropped along with the unused
// constructor params they were referenced by.

class EpubImageCache {
 public:
  // v2.0.148 — class-scoped (was file-scope).  See note in v2.0.148 changelog.
  // kMaxWordSize is still referenced by external translation units (legacy
  // include path), so keep both.
  static constexpr int kMaxWordSize = 200;
  static constexpr int kMaxXmlDepth = 100;

  enum class ReadItemStatus : uint8_t {
    Success,
    Aborted,
    NotFound,
    ArchiveError,
    IoError,
    WriteError,
  };

  enum class CachedImageStatus : uint8_t {
    Success,
    RetryableInterruption,
    TerminalFailure,
  };

  enum class ImageFailureClass : uint8_t {
    None,
    AbortRequested,
    Timeout,
    LowMemory,
    DataUri,
    UnsupportedFormat,
    CacheHit,
    CachedOpenFailed,
    CachedBmpInvalid,
    TempOpenFailed,
    ExtractAborted,
    ExtractFailed,
    ConvertAborted,
    ConvertFailed,
    GeneratedBmpInvalid,
    MissingSrc,
    ReadItemUnavailable,
    ImageCacheDisabled,
  };

  // v2.0.148 — public lazy image-cache entry point for the streaming
  // render path.  Idempotent: cache hit returns existing BMP path +
  // dimensions; cache miss extracts JPEG/PNG from EPUB ZIP via
  // `readItemFn`, decodes to BMP on LittleFS, and returns the new path.
  // Returns false on missing src, decode failure, abort, or low memory.
  bool cacheImageForStreaming(const std::string& src, std::string& outBmpPath,
                                uint16_t& outWidth, uint16_t& outHeight);

  // v2.0.171 — clean image-cache-only constructor.  Dropped parser-era
  // params (filepath, completePageFn, progressFn, cssParser) — they were
  // kept as ignored args after v2.0.164's parser-loop deletion only
  // because we didn't want to touch ReaderState in that build.  Now
  // that the class is renamed, the call site updates anyway.
  explicit EpubImageCache(
      GfxRenderer& renderer, const RenderConfig& config,
      const std::string& chapterBasePath, const std::string& imageCachePath,
      const std::function<ReadItemStatus(const std::string&, Print&, size_t,
                                          const std::function<bool()>&)>& readItemFn,
      bool quickImageDecode = false,
      const std::function<bool()>& externalAbortCallback = nullptr)
      : renderer(renderer),
        config(config),
        chapterBasePath(chapterBasePath),
        imageCachePath(imageCachePath),
        readItemFn(readItemFn),
        quickImageDecode_(quickImageDecode),
        externalAbortCallback_(externalAbortCallback) {}
  ~EpubImageCache();

 private:
  GfxRenderer& renderer;
  RenderConfig config;
  std::string chapterBasePath;
  std::string imageCachePath;
  std::function<ReadItemStatus(const std::string&, Print&, size_t,
                                const std::function<bool()>&)> readItemFn;
  bool quickImageDecode_ = false;
  std::function<bool()> externalAbortCallback_;
  uint8_t consecutiveImageFailures_ = 0;

  // v2.0.164 — these flags used to be set by the parser's cooperative-
  // suspend path; with the parser gone, they remain `false` for the
  // lifetime of the instance.  `cacheImage` still reads them in its
  // abort classifier, so we keep the fields rather than rewriting that
  // logic — costs 2 bytes, simpler diff.
  bool stopRequested_ = false;
  bool cooperativeAbortRequested_ = false;

  struct CachedImageResult {
    std::string cachedBmpPath;
    std::string resolvedPath;
    std::string failureReason;
    uint16_t width = 0;
    uint16_t height = 0;
    bool cacheHit = false;
    bool success = false;
    bool retryable = false;
    CachedImageStatus status = CachedImageStatus::TerminalFailure;
    ImageFailureClass failureClass = ImageFailureClass::None;
  };

  // v2.0.78: see implementation for rationale on the per-image budgets.
  static constexpr uint32_t MAX_QUICK_IMAGE_PROCESS_TIME_MS = 15000;
  static constexpr uint32_t MAX_FULL_IMAGE_PROCESS_TIME_MS = 25000;
  static constexpr size_t MIN_IMAGE_PROCESS_FREE_HEAP = 4096;

  static std::unordered_set<size_t>& sessionFailedImageHashes() {
    static std::unordered_set<size_t> instance;
    return instance;
  }

  static bool validateCachedBmp(const std::string& cachedBmpPath, uint16_t& width, uint16_t& height,
                                std::string& failureReason);
  CachedImageResult cacheImage(const std::string& src);
};
