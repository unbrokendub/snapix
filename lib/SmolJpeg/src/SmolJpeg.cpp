#include "SmolJpeg.h"

#include "SmolJpegDecode.h"
#include "SmolJpegParse.h"
#include "SmolJpegState.h"

#include <new>

#if defined(ARDUINO)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace snapix::smoljpeg {

// =============================================================================
// Zig-zag scan order (JPEG spec table T.81 Annex F.2 / figure F.6).
// 64 entries; ZZ[i] gives the natural-order position of the i'th
// coefficient in the zig-zag-decoded block.
// =============================================================================
const uint8_t kZigZag[64] = {
   0,  1,  8, 16,  9,  2,  3, 10,
  17, 24, 32, 25, 18, 11,  4,  5,
  12, 19, 26, 33, 40, 48, 41, 34,
  27, 20, 13,  6,  7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36,
  29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46,
  53, 60, 61, 54, 47, 55, 62, 63,
};

const char* statusToString(const Status s) {
  switch (s) {
    case Status::Ok:                   return "ok";
    case Status::NotImplemented:       return "not-implemented";
    case Status::InputError:           return "input-error";
    case Status::OutputError:          return "output-error";
    case Status::AllocFailed:          return "alloc-failed";
    case Status::InvalidJpeg:          return "invalid-jpeg";
    case Status::UnsupportedFeature:   return "unsupported-feature";
    case Status::ProgressiveScanLimit: return "progressive-scan-limit";
    case Status::ExceedsPixelLimit:    return "exceeds-pixel-limit";
    case Status::Aborted:              return "aborted";
  }
  return "?";
}

// =============================================================================
// Public API.  Decode bottoms out on a single BSS-resident `JpegState`
// instance protected by a FreeRTOS mutex (host-side builds skip the
// mutex altogether — host tests are single-threaded).
//
// v2.0.92 (Phase 4c-partial — heap-fragmentation reduction): the previous
// implementation `new (std::nothrow) JpegState()` per decode meant every
// JPEG conversion competed for a contiguous ~8.4 KB heap block with the
// rest of the system.  On a chapter parse + concurrent inline-image
// drain that ~8.4 KB transient peak landed right when expat / ZIP
// inflate / chapter-cache writes were all peaking too — and the cover
// gen call site reported `AllocFailed` on the user's hardware (see
// v2.0.89 `SmolJpeg.h` history).  Pooling the state in BSS:
//   * eliminates the 8.4 KB heap allocation entirely on the decode path
//   * costs +8.4 KB of always-pinned BSS in v3_alpha
//   * costs zero BSS in default env (the function is unreachable from
//     a flag-off build and the linker drops the static via LTO)
//   * serialises concurrent decoders via mutex (cover gen on loopTask
//     vs PendingImageDecode drain on the BG cache task) — they were
//     already temporally separated in practice but the lock keeps the
//     contract honest.
// =============================================================================

namespace {

// BSS-resident shared decoder state.  ~8.4 KB — the largest single
// member of the workspace family.  Single-instance because decode is
// serialised by `gJpegMutex` below; the next caller resets `width=0` /
// quant tables / Huffman tables via `parseMarkers` before use.
JpegState& sharedJpegState() {
  static JpegState gSharedJpegState;
  return gSharedJpegState;
}

#if defined(ARDUINO)
// Mutex protecting `sharedJpegState()`.  Lazy-init via a function-local
// static — C++11 guarantees thread-safe initialisation.  Acquisition is
// uncontended in practice (cover gen runs at boot, PendingImageDecode
// drain runs on a separate task once book navigation is active); the
// lock is defence-in-depth.
SemaphoreHandle_t& jpegMutex() {
  static SemaphoreHandle_t h = xSemaphoreCreateMutex();
  return h;
}

struct ScopedJpegLock {
  bool owned = false;
  ScopedJpegLock() {
    SemaphoreHandle_t m = jpegMutex();
    if (m && xSemaphoreTake(m, portMAX_DELAY) == pdTRUE) {
      owned = true;
    }
  }
  ~ScopedJpegLock() {
    if (owned) xSemaphoreGive(jpegMutex());
  }
};
#else
// Host-side: tests are single-threaded; no lock needed.
struct ScopedJpegLock {
  bool owned = true;
};
#endif

}  // namespace

Status decodeTo1BitBmp(InputStream& in, OutputStream& bmpOut, const int maxW,
                       const int maxH, bool (*shouldAbort)()) {
  ScopedJpegLock lock;
  if (!lock.owned) return Status::AllocFailed;  // mutex creation failed

  JpegState& state = sharedJpegState();
  // parseMarkers zeros the state via `state = JpegState{};` as its first
  // step so any leftover bytes from a prior decode are wiped before use.
  Status s = parseMarkers(in, state);
  if (s == Status::Ok) {
    s = decodeBaselineStream(in, state, maxW, maxH, bmpOut, shouldAbort);
  }
  return s;
}

Status peekDimensions(InputStream& in, uint16_t& outWidth, uint16_t& outHeight) {
  return scanSofDimensions(in, outWidth, outHeight);
}

}  // namespace snapix::smoljpeg
