#include "SmolPngDecode.h"

#include "SmolPngBmp.h"
#include "SmolPngDither.h"
#include "SmolPngFilter.h"
#include "SmolPngFormat.h"
#include "SmolPngIdatReader.h"

extern "C" {
#include "uzlib.h"
}

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace snapix::smolpng {

namespace {

// Owning RAII wrapper for `new (std::nothrow) T[]`.
template <typename T>
struct OwnedArray {
  T* ptr = nullptr;
  OwnedArray() = default;
  explicit OwnedArray(const size_t n) : ptr(new (std::nothrow) T[n]()) {}
  ~OwnedArray() { delete[] ptr; }
  OwnedArray(const OwnedArray&) = delete;
  OwnedArray& operator=(const OwnedArray&) = delete;
  T* get() const { return ptr; }
  explicit operator bool() const { return ptr != nullptr; }
};

// Wrapper struct lets us recover the IdatReader from a uzlib_uncomp
// pointer in the source-read callback.  uz MUST be the first member.
struct PngDecompressor {
  uzlib_uncomp uz;
  IdatReader*  idat;
};

int pngSourceCb(uzlib_uncomp* d) {
  PngDecompressor* pd = reinterpret_cast<PngDecompressor*>(d);
  return pd->idat->nextByte();
}

// Rec. 601 RGB → luma (integer approximation).  Y = round(0.299R + 0.587G + 0.114B).
// Using 8-bit fixed-point coefficients (sum = 256 exactly).
inline uint8_t rgbLuma(const int r, const int g, const int b) {
  return static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
}

// Convert one unfiltered RGB / RGBA scanline to single-byte luma.
// `src` and `dst` must not overlap unless `dst == src` (in-place);
// in-place is safe because dst[x] writes only after src[x*bpp] is read.
// RGBA alpha is blended against a white background (paper-like).
void rgbToLuma(const uint8_t* src, uint8_t* dst, const size_t width,
                const uint8_t bpp) {
  if (bpp == 3) {
    for (size_t x = 0; x < width; ++x) {
      dst[x] = rgbLuma(src[x * 3], src[x * 3 + 1], src[x * 3 + 2]);
    }
  } else {  // bpp == 4 (RGBA)
    for (size_t x = 0; x < width; ++x) {
      const int r = src[x * 4];
      const int g = src[x * 4 + 1];
      const int b = src[x * 4 + 2];
      const int a = src[x * 4 + 3];
      // Pre-multiply: blended = (rgb * a + 255 * (255 - a)) / 255
      const int yRaw = (77 * r + 150 * g + 29 * b) >> 8;
      const int blended = (yRaw * a + 255 * (255 - a)) / 255;
      dst[x] = static_cast<uint8_t>(blended);
    }
  }
}

int computeScale(const int srcW, const int srcH, const int maxW,
                  const int maxH) {
  int s = 1;
  if (maxW > 0 && srcW > maxW) s = std::max(s, (srcW + maxW - 1) / maxW);
  if (maxH > 0 && srcH > maxH) s = std::max(s, (srcH + maxH - 1) / maxH);
  return s;
}

}  // namespace

Status decodeBaselineStream(InputStream& in, PngState& state, const int maxW,
                             const int maxH, OutputStream& out,
                             bool (*shouldAbort)()) {
  if (state.height == 0 || state.width == 0) return Status::InvalidPng;
  if (state.rowStride == 0) return Status::InvalidPng;
  if (state.firstIdatOffset == 0) return Status::InvalidPng;

  const int srcW = static_cast<int>(state.width);
  const int srcH = static_cast<int>(state.height);
  const int scale = computeScale(srcW, srcH, maxW, maxH);
  const int outW = std::max(1, srcW / scale);
  const int outH = std::max(1, srcH / scale);

  const size_t scanlineSize = 1 + state.rowStride;
  // 32 KB dict ring buffer required for deflate back-references in zlib's
  // 32 KB window mode (which PNG always uses).  Matches the existing
  // InflateReader convention in lib/InflateReader/.
  static constexpr size_t kInflateDictSize = 32768;
  OwnedArray<uint8_t>  curBuf(scanlineSize);
  OwnedArray<uint8_t>  prevBuf(scanlineSize);
  OwnedArray<uint8_t>  lumaBuf(state.width);
  OwnedArray<int16_t>  errCur(static_cast<size_t>(outW + 2));
  OwnedArray<int16_t>  errNxt(static_cast<size_t>(outW + 2));
  const uint32_t outStride = bmp1BitRowStride(static_cast<uint32_t>(outW));
  OwnedArray<uint8_t>  outRowBuf(outStride);
  OwnedArray<uint8_t>  dictRing(kInflateDictSize);
  if (!curBuf || !prevBuf || !lumaBuf || !errCur || !errNxt ||
      !outRowBuf || !dictRing) {
    return Status::AllocFailed;
  }

  // BMP header.
  if (!writeBmp1BitHeader(out, static_cast<uint32_t>(outW),
                          static_cast<uint32_t>(outH))) {
    return Status::OutputError;
  }

  // Set up uzlib over the IDAT chunk stitcher.
  //
  // Stack-pressure note: `PngDecompressor` embeds `uzlib_uncomp`, which is
  // ~2 KB (Huffman LUTs).  Plus `IdatReader` has a 128-B internal buffer.
  // Placing these on the loopTask stack tipped the cover-gen call chain
  // over the 8-KB stack budget on ESP32-C3 in early v2.0.88 builds.  We
  // heap-allocate both to keep the function frame small.  unique_ptr
  // dtor releases on every exit path.
  auto idatStorage = std::unique_ptr<IdatReader>(
      new (std::nothrow) IdatReader(in, state.firstIdatOffset));
  auto pdStorage = std::unique_ptr<PngDecompressor>(
      new (std::nothrow) PngDecompressor());
  if (!idatStorage || !pdStorage) return Status::AllocFailed;
  IdatReader&      idat = *idatStorage;
  PngDecompressor& pd   = *pdStorage;
  pd.idat = &idat;
  uzlib_uncompress_init(&pd.uz, dictRing.get(), kInflateDictSize);
  pd.uz.source = nullptr;
  pd.uz.source_limit = nullptr;
  pd.uz.source_read_cb = pngSourceCb;

  // Skip the 2-byte zlib header (CMF + FLG).  We trust the file
  // structure — same shortcut snapix's InflateReader takes.
  uzlib_get_byte(&pd.uz);
  uzlib_get_byte(&pd.uz);

  uint8_t* curPtr  = curBuf.get();
  uint8_t* prevPtr = prevBuf.get();
  // First-row "previous" is implicitly all zeros (and OwnedArray<uint8_t>
  // zero-init'd via value-init, so prevPtr already holds zeros).

  int rowsEmitted = 0;
  for (uint32_t y = 0; y < state.height && rowsEmitted < outH; ++y) {
    pd.uz.dest       = curPtr;
    pd.uz.dest_start = curPtr;
    pd.uz.dest_limit = curPtr + scanlineSize;

    const int rc = uzlib_uncompress(&pd.uz);
    if (rc != TINF_OK && rc != TINF_DONE) return Status::InvalidPng;
    if (pd.uz.dest != pd.uz.dest_limit) return Status::InvalidPng;

    const uint8_t filterType = curPtr[0];
    unfilterScanline(curPtr + 1, prevPtr + 1, state.rowStride,
                     state.bytesPerPixel, filterType);

    // Emit only every `scale`-th row (vertical downscale).
    if (static_cast<int>(y) % scale == 0 && rowsEmitted < outH) {
      rgbToLuma(curPtr + 1, lumaBuf.get(), state.width, state.bytesPerPixel);
      std::memset(outRowBuf.get(), 0, outStride);
      ditherRowGrey(lumaBuf.get(), static_cast<size_t>(scale),
                    static_cast<size_t>(outW), errCur.get(), errNxt.get(),
                    outRowBuf.get());
      if (!out.write(outRowBuf.get(), outStride)) return Status::OutputError;
      // Swap error rows for next iteration.
      std::swap(errCur.ptr, errNxt.ptr);
      std::memset(errNxt.get(), 0,
                  sizeof(int16_t) * static_cast<size_t>(outW + 2));
      ++rowsEmitted;
    }

    // Swap cur ↔ prev: next row's filter context is THIS row's unfiltered data.
    std::swap(curPtr, prevPtr);

    if (shouldAbort && shouldAbort()) return Status::Aborted;
  }

  return Status::Ok;
}

}  // namespace snapix::smolpng
