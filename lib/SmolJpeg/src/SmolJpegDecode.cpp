#include "SmolJpegDecode.h"

#include "SmolJpegBitReader.h"
#include "SmolJpegBlock.h"
#include "SmolJpegBmp.h"
#include "SmolJpegDither.h"
#include "SmolJpegHuffman.h"
#include "SmolJpegIDCT.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

#define TAG "SMOL_JPEG"

namespace snapix::smoljpeg {

namespace {

// Owning RAII wrapper for `new (std::nothrow) T[]`; matches std::unique_ptr
// API closely enough for our uses but avoids pulling in the heavier <memory>
// surface for embedded builds.
template <typename T>
struct OwnedArray {
  T* ptr = nullptr;
  OwnedArray() = default;
  explicit OwnedArray(const size_t n) : ptr(new (std::nothrow) T[n]()) {}
  ~OwnedArray() { delete[] ptr; }
  OwnedArray(const OwnedArray&) = delete;
  OwnedArray& operator=(const OwnedArray&) = delete;
  // v2.0.100: move semantics so an OwnedArray can be assigned from a
  // temporary (e.g. `yRowHeap = OwnedArray<uint8_t>(N)` in the yRow
  // pool-overflow fallback path).  Without these the compiler tries to
  // use the deleted copy assignment.
  OwnedArray(OwnedArray&& other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
  OwnedArray& operator=(OwnedArray&& other) noexcept {
    if (this != &other) {
      delete[] ptr;
      ptr = other.ptr;
      other.ptr = nullptr;
    }
    return *this;
  }
  T* get() const { return ptr; }
  explicit operator bool() const { return ptr != nullptr; }
  T& operator[](const size_t i) const { return ptr[i]; }
};

// Integer downscale factor: max(ceil(srcW/maxW), ceil(srcH/maxH), 1).
int computeScale(const int srcW, const int srcH, const int maxW,
                 const int maxH) {
  int s = 1;
  if (maxW > 0 && srcW > maxW) s = std::max(s, (srcW + maxW - 1) / maxW);
  if (maxH > 0 && srcH > maxH) s = std::max(s, (srcH + maxH - 1) / maxH);
  return s;
}

// =============================================================================
// yRow allocation strategy.
//
// `yRow` is the MCU-row luminance scratch buffer; size is `alignedW × mcuH`
// bytes — typical inline image ~10-13 KB, largest observed cover ~16.6 KB.
//
// Earlier prototype (v2.0.100) pinned a 16 KB BSS pool `gYRow[16384]` to
// avoid the transient heap spike during chapter-parse+inline-decode
// overlap.  Hardware bring-up exposed the architectural mistake: that 16 KB
// was permanently subtracted from the runtime heap budget (163 → 147 KB),
// which then collapsed `largest_free` below `MIN_LAYOUT_FREE_HEAP=10240`
// on the new-EPUB cold-open path — the parser aborted before completing
// its first page.  Pinning transient scratch in BSS solves one symptom
// (decode-time spike) by creating a permanent worse one (-16 KB budget).
//
// v2.0.101: yRow is back on the heap, allocated once per decode via
// `OwnedArray<uint8_t>`.  The transient-spike problem is addressed at its
// real root in Phase 4c (per-block layout scratch in workspace BSS) and
// by `releaseAllPersistent()` after cover-gen — both eliminate the
// fragmentation that made the transient yRow alloc fail in the first
// place.  Concurrency: `decodeBaselineStream` runs under the JpegState
// `ScopedJpegLock` (SmolJpeg.cpp), so at most one decode is active.
// =============================================================================

}  // namespace

Status decodeBaselineStream(InputStream& in, JpegState& state, const int maxW,
                            const int maxH, OutputStream& out,
                            bool (*shouldAbort)()) {
  if (state.progressive) {
    LOG_INF(TAG, "decodeBaseline: progressive scan unsupported");
    return Status::ProgressiveScanLimit;
  }
  if (state.numComp == 0 || state.scanNumComp == 0) {
    LOG_INF(TAG, "decodeBaseline: comp counts zero numComp=%u scanNumComp=%u w=%u h=%u",
            static_cast<unsigned>(state.numComp),
            static_cast<unsigned>(state.scanNumComp),
            static_cast<unsigned>(state.width),
            static_cast<unsigned>(state.height));
    return Status::InvalidJpeg;
  }

  // ---- pre-flight validation: every referenced table must be loaded ----
  for (uint8_t i = 0; i < state.scanNumComp; ++i) {
    const Component& c = state.comp[state.scanOrder[i]];
    if (!state.qtOk[c.qtIdx]) {
      LOG_INF(TAG, "decodeBaseline: missing QT scanIdx=%u compId=%u qtIdx=%u qtOk=[%u,%u,%u,%u]",
              static_cast<unsigned>(i), static_cast<unsigned>(c.id),
              static_cast<unsigned>(c.qtIdx),
              static_cast<unsigned>(state.qtOk[0]),
              static_cast<unsigned>(state.qtOk[1]),
              static_cast<unsigned>(state.qtOk[2]),
              static_cast<unsigned>(state.qtOk[3]));
      return Status::InvalidJpeg;
    }
    if (!state.dcOk[c.dcTbl]) {
      LOG_INF(TAG, "decodeBaseline: missing DHT-DC scanIdx=%u compId=%u dcTbl=%u dcOk=[%u,%u,%u,%u]",
              static_cast<unsigned>(i), static_cast<unsigned>(c.id),
              static_cast<unsigned>(c.dcTbl),
              static_cast<unsigned>(state.dcOk[0]),
              static_cast<unsigned>(state.dcOk[1]),
              static_cast<unsigned>(state.dcOk[2]),
              static_cast<unsigned>(state.dcOk[3]));
      return Status::InvalidJpeg;
    }
    if (!state.acOk[c.acTbl]) {
      LOG_INF(TAG, "decodeBaseline: missing DHT-AC scanIdx=%u compId=%u acTbl=%u acOk=[%u,%u,%u,%u]",
              static_cast<unsigned>(i), static_cast<unsigned>(c.id),
              static_cast<unsigned>(c.acTbl),
              static_cast<unsigned>(state.acOk[0]),
              static_cast<unsigned>(state.acOk[1]),
              static_cast<unsigned>(state.acOk[2]),
              static_cast<unsigned>(state.acOk[3]));
      return Status::InvalidJpeg;
    }
  }

  // ---- output dims ----
  const int srcW = state.width;
  const int srcH = state.height;
  const int scale = computeScale(srcW, srcH, maxW, maxH);
  const int outW = std::max(1, srcW / scale);
  const int outH = std::max(1, srcH / scale);

  // ---- MCU grid ----
  const int mcuW = state.maxH * 8;
  const int mcuH = state.maxV * 8;
  const int numMcuX = (srcW + mcuW - 1) / mcuW;
  const int numMcuY = (srcH + mcuH - 1) / mcuH;
  // Buffer width aligns to MCU boundary; over-decoded right-edge pixels are
  // just left unused.
  const int alignedW = numMcuX * mcuW;

  // ---- transient buffers ----
  // v2.0.101: yRow on heap (see comment block at top of file for rationale —
  // the BSS-pool prototype regressed the cold-open heap budget).
  const size_t yRowSize = static_cast<size_t>(alignedW) * mcuH;
  OwnedArray<uint8_t> yRowHeap(yRowSize);
  if (!yRowHeap) return Status::AllocFailed;
  uint8_t* const yRow = yRowHeap.get();
  OwnedArray<int16_t>  errCur(static_cast<size_t>(outW + 2));
  OwnedArray<int16_t>  errNxt(static_cast<size_t>(outW + 2));
  const uint32_t outStride = bmp1BitRowStride(static_cast<uint32_t>(outW));
  OwnedArray<uint8_t>  outBuf(static_cast<size_t>(outStride));
  if (!errCur || !errNxt || !outBuf) return Status::AllocFailed;

  // ---- BMP header ----
  if (!writeBmp1BitHeader(out, static_cast<uint32_t>(outW),
                          static_cast<uint32_t>(outH))) {
    return Status::OutputError;
  }

  // ---- entropy decode ----
  //
  // Stack-pressure note: the cover-gen call chain on loopTask (Arduino
  // default 8 KB stack) already burns ~5 KB before reaching here.  In an
  // earlier v2.0.88 build, `BitReader` (~280 B with its 256-B buf_) plus
  // an inner-loop `int32_t coef[64]` (256 B), `uint8_t pix[64]` (64 B),
  // decodeBaselineBlock's own scratch `int32_t coef[64]` (256 B), and
  // idct's `int32_t ws[64]` (256 B) summed to ~1.1 KB extra stack —
  // enough to tip a real 1026×1500 cover JPEG into a stack-protection
  // panic.  We now allocate every one of these on the heap once at the
  // top of decode and re-use across all MCUs.  Net stack savings:
  // ~1.1 KB at the deepest call site (decodeBaselineBlock → idct).
  std::unique_ptr<BitReader> brStorage(new (std::nothrow) BitReader(
      in, state.scanStart, in.length()));
  OwnedArray<int32_t> coefBuf(64);
  OwnedArray<int32_t> blockZigzag(64);
  OwnedArray<int32_t> idctScratch(64);
  OwnedArray<uint8_t> pixBuf(64);
  if (!brStorage || !coefBuf || !blockZigzag || !idctScratch || !pixBuf) {
    return Status::AllocFailed;
  }
  BitReader& br = *brStorage;

  int32_t dcPredictor[kMaxComponents] = {0, 0, 0, 0};
  int restartCounter = 0;
  int rowsEmitted = 0;

  // Index into scanOrder that we treat as luma — convention: first.
  const uint8_t lumaScanIdx = state.scanOrder[0];

  for (int my = 0; my < numMcuY && rowsEmitted < outH; ++my) {
    // Clear the Y row buffer (over-decoded edge pixels otherwise stale).
    std::memset(yRow, 0,
                static_cast<size_t>(alignedW) * static_cast<size_t>(mcuH));

    for (int mx = 0; mx < numMcuX; ++mx) {
      // For each component in scan order:
      for (uint8_t ci = 0; ci < state.scanNumComp; ++ci) {
        const uint8_t compIdx = state.scanOrder[ci];
        const Component& comp = state.comp[compIdx];
        const HuffTable& dcT = state.dcHuff[comp.dcTbl];
        const HuffTable& acT = state.acHuff[comp.acTbl];
        const uint16_t* qt   = state.qt[comp.qtIdx];
        const bool isLuma = (compIdx == lumaScanIdx);

        for (int by = 0; by < comp.vSamp; ++by) {
          for (int bx = 0; bx < comp.hSamp; ++bx) {
            const Status s = decodeBaselineBlock(br, dcT, acT, qt,
                                                  dcPredictor[compIdx],
                                                  coefBuf.get(),
                                                  blockZigzag.get());
            if (s != Status::Ok) return s;

            if (isLuma) {
              idct(coefBuf.get(), pixBuf.get(), idctScratch.get());
              // Write the 8×8 block into yRow at (px, py).  Y is currently
              // the only luma component so its sampling factors map 1:1 to
              // pixels.  (Real chroma subsampling would put 16×16 Y blocks
              // in for 4:2:0, etc.; for 4:4:4 / 4:2:2 / 4:2:0 luma it's
              // always (bx*8, by*8).)
              const int px = mx * mcuW + bx * 8;
              const int py = by * 8;
              for (int j = 0; j < 8; ++j) {
                const int dstOff = (py + j) * alignedW + px;
                std::memcpy(&yRow[dstOff], &pixBuf[j * 8], 8);
              }
            }
          }
        }
      }

      // Restart-marker handling: every `restartInterval` MCUs the encoder
      // pads to byte boundary, emits RSTn, and resets DC predictors.
      //
      // v2.0.108 (root cause fix for FB2 inline-image "bad restart
      // marker=0x00" failures): pre-fix, we just called consumeBits to
      // drop leftover bits, then checked pendingMarker().  But marker_
      // is only set when nextDataByte() ACTUALLY FETCHES a 0xFF Dn
      // sequence from the source.  If the previous MCU's Huffman codes
      // consumed bytes RIGHT UP TO (but not past) the 0xFF marker prefix,
      // marker_ was still 0 from before, and we wrongly reported the
      // JPEG as invalid.  This was intermittent — some JPEGs happened
      // to over-fetch into the marker via Huffman code lookahead and
      // worked fine; others stopped exactly on the byte boundary and
      // triggered the failure.  Observed on Calibre FB2 inline images
      // like img_28 (240x185 grayscale) which have restartInterval>0
      // and 16 MCUs per row — the last MCU before each row's restart
      // boundary frequently aligned exactly.
      //
      // Fix: after consumeBits, force one byte fetch via ensure(1).
      // That drives nextDataByte() which sees 0xFF Dn, sets marker_,
      // and returns -1.  ensure() pads bitBuf_ with 1-bits since
      // marker_ is now set; we drop that padding (it's not real data)
      // before resuming entropy decode for the next MCU.
      if (state.restartInterval > 0) {
        ++restartCounter;
        if (restartCounter == state.restartInterval &&
            (mx + 1 < numMcuX || my + 1 < numMcuY)) {
          restartCounter = 0;
          // 1) Drop any leftover bits from the just-finished MCU.
          br.consumeBits(br.bitCount());
          // 2) Force the BitReader to fetch the next byte so it scans
          //    forward to the 0xFF Dn marker prefix.  If found,
          //    marker_ is set and ensure() returns with 8 bits of
          //    1-padding (which we discard below before next decode).
          br.ensure(1);
          const uint8_t marker = br.pendingMarker();
          if (marker < kMarkerRST0 || marker > kMarkerRST7) {
            LOG_INF(TAG, "decodeBaseline: bad restart marker=0x%02X mx=%d my=%d",
                    static_cast<unsigned>(marker), mx, my);
            return Status::InvalidJpeg;
          }
          br.clearMarker();
          // 3) Drop the 1-padding ensure() injected on marker hit.  The
          //    real next-MCU data starts at the byte AFTER the marker —
          //    nextDataByte() on the next ensure() will pull it cleanly.
          br.consumeBits(br.bitCount());
          for (size_t i = 0; i < kMaxComponents; ++i) dcPredictor[i] = 0;
        }
      }
    }  // mx

    // Dither and emit (outH / numMcuY rows worth) for this MCU row.
    for (int sy = 0; sy < mcuH && rowsEmitted < outH; ++sy) {
      if (sy % scale != 0) continue;
      const int absY = my * mcuH + sy;
      if (absY >= srcH) break;

      std::memset(outBuf.get(), 0, outStride);
      ditherRowGrey(&yRow[sy * alignedW], static_cast<size_t>(scale),
                    static_cast<size_t>(outW), errCur.get(), errNxt.get(),
                    outBuf.get());
      if (!out.write(outBuf.get(), outStride)) return Status::OutputError;

      // Swap error rows for next iteration; clear the new "next" row.
      std::swap(errCur.ptr, errNxt.ptr);
      std::memset(errNxt.get(), 0,
                  sizeof(int16_t) * static_cast<size_t>(outW + 2));
      ++rowsEmitted;
    }

    if (shouldAbort && shouldAbort()) return Status::Aborted;
  }

  return Status::Ok;
}

}  // namespace snapix::smoljpeg
