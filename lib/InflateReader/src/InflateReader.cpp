#include "InflateReader.h"

#include <cstring>
#include <type_traits>

namespace {
constexpr size_t INFLATE_DICT_SIZE = 32768;

// v2.0.178 — REVERT of v2.0.177's 32 KB BSS scratch buffer.  The math
// in v2.0.177's changelog assumed "min free heap unchanged because we
// just relocate 32 KB from heap-transient to BSS-permanent".  Hardware
// testing showed that assumption was wrong: the peak transient working
// set during a chapter parse is HIGHER than I estimated because
//   * JPEG decoder instance (25 KB) stays pinned in heap after the first
//     cover decode — I dismissed this as "already pinned" but it
//     subtracts from the budget available to OTHER allocations during
//     chapter parse.
//   * Vaghnye_gody EPUB metadata has TWO CSS files (43 + 39 rules) plus
//     51 spine items plus 27 TOC entries — pins ~25-30 KB persistently
//     once loaded, more than the single-CSS EPUBs I was sizing against.
//   * FB2 image rendering during cache cold-extend adds another ~5-10 KB
//     of in-flight state (image cache, paginator workspace, markerize
//     chunk buffer, ParsedText spill buffer setup).
//
// On v2.0.177 hardware, the user's FB2 spine=46 page-turn through an
// image-bearing section bottomed out at largest=8180 bytes during cold
//   extend → ParsedText tried to spill → underlying allocation failed →
//   abort() was called at PC 0x421670cb on core 0
//   (panic + reboot, log: free=21700 largest=8180 → spill → abort)
//
// The v2.0.176 three-tier strategy (heap first, externalBuffer fallback)
// is correct.  Restoring it.  The display-corruption glitch from
// framebuffer-as-scratch is a smaller user impact than panic+reboot, and
// in practice the heap-first preference avoids it on the fast path.
//
// LESSON FOR FUTURE BSS MIGRATIONS:
// Don't trust the "peak transient minus relocated allocation" math.
// Measure on hardware against the WORST realistic workload (multi-CSS
// EPUB + cached JPEG instance + image-bearing FB2 + cold extend) BEFORE
// trusting the budget.  The headroom we have on simple workloads
// vanishes on heavy ones.
}

// Guarantee the cast pattern in the header comment is valid.
static_assert(std::is_standard_layout<InflateReader>::value,
              "InflateReader must be standard-layout for the uzlib callback cast to work");

InflateReader::~InflateReader() { deinit(); }

bool InflateReader::init(const bool streaming) { return init(streaming, nullptr); }

bool InflateReader::init(const bool streaming, uint8_t* externalBuffer) {
  deinit();

  if (streaming) {
    // v2.0.178 — back to v2.0.176 strategy: heap first, externalBuffer
    // fallback only when heap can't provide 32 KB contiguous.  See the
    // top-of-file comment for why v2.0.177's BSS-primary approach was
    // reverted (insufficient post-BSS heap headroom for FB2 image-bearing
    // workloads).
    ringBuffer = static_cast<uint8_t*>(malloc(INFLATE_DICT_SIZE));
    if (ringBuffer) {
      ownsRingBuffer = true;
    } else if (externalBuffer) {
      // Heap fragmented — accept the framebuffer-corruption glitch risk
      // (right-half panel garbage during chapter parse, fires only when
      // the e-paper driver's deferred RED-RAM upload reads the buffer
      // mid-inflate).  Better than failing the chapter parse entirely.
      ringBuffer = externalBuffer;
      ownsRingBuffer = false;
    } else {
      return false;
    }
    memset(ringBuffer, 0, INFLATE_DICT_SIZE);
  }

  uzlib_uncompress_init(&decomp, ringBuffer, ringBuffer ? INFLATE_DICT_SIZE : 0);
  return true;
}

void InflateReader::deinit() {
  // v2.0.178 — back to two release paths:
  //   * Heap (owned): free the malloc'd block.
  //   * External buffer (not owned): nothing to do; caller owns lifetime.
  if (ringBuffer && ownsRingBuffer) {
    free(ringBuffer);
  }
  ringBuffer = nullptr;
  ownsRingBuffer = false;
  memset(&decomp, 0, sizeof(decomp));
}

void InflateReader::setSource(const uint8_t* src, size_t len) {
  decomp.source = src;
  decomp.source_limit = src + len;
}

void InflateReader::setReadCallback(int (*cb)(struct uzlib_uncomp*)) { decomp.source_read_cb = cb; }

void InflateReader::skipZlibHeader() {
  uzlib_get_byte(&decomp);
  uzlib_get_byte(&decomp);
}

bool InflateReader::read(uint8_t* dest, size_t len) {
  if (!ringBuffer) {
    decomp.dest_start = dest;
  }
  decomp.dest = dest;
  decomp.dest_limit = dest + len;

  const int res = uzlib_uncompress(&decomp);
  if (res < 0) return false;
  return decomp.dest == decomp.dest_limit;
}

InflateStatus InflateReader::readAtMost(uint8_t* dest, size_t maxLen, size_t* produced) {
  if (!ringBuffer) {
    decomp.dest_start = dest;
  }
  decomp.dest = dest;
  decomp.dest_limit = dest + maxLen;

  const int res = uzlib_uncompress(&decomp);
  *produced = static_cast<size_t>(decomp.dest - dest);

  if (res == TINF_DONE) return InflateStatus::Done;
  if (res < 0) return InflateStatus::Error;
  return InflateStatus::Ok;
}
