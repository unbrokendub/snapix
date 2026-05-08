#pragma once

#include <JPEGDEC.h>

#include <cstdint>
#include <functional>

class FsFile;
class Print;
class ZipFile;

class JpegToBmpConverter {
  static unsigned char jpegReadCallback(unsigned char* pBuf, unsigned char buf_size,
                                        unsigned char* pBytes_actually_read, void* pCallback_data);
  static bool jpegFileToBmpStreamInternal(class FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool quickMode = false,
                                          const std::function<bool()>& shouldAbort = nullptr);

 public:
  static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                          const std::function<bool()>& shouldAbort = nullptr);
  // Convert to 1-bit BMP (black and white only, no grays)
  static bool jpegFileTo1BitBmpStream(FsFile& jpegFile, Print& bmpOut);
  // Convert to 1-bit BMP with custom target size (for thumbnails)
  static bool jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Quick preview mode: simple threshold instead of dithering (faster but lower quality)
  static bool jpegFileToBmpStreamQuick(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                       const std::function<bool()>& shouldAbort = nullptr);
  // Header-only peek: returns true on success, fills width/height from JPEG SOF marker.
  // ~10× cheaper than a full decode — used by FB2 fast-mode image registration so the
  // page layout has correct dimensions before the BG worker decodes pixels.
  static bool peekDimensions(FsFile& jpegFile, int& outWidth, int& outHeight);

  // Tiny preview decode: picojpeg's reduce=1 mode skips AC dequant + IDCT + chroma
  // upsampling, producing a 1-pixel-per-MCU image that's 5-10× faster than a full
  // decode but at 1/8 linear resolution (8× pixelated when displayed at full size).
  // Used by the BG worker to flash a blurry preview within ~1 s before swapping in
  // the full-quality decode.  Output BMP dimensions are MCUS-per-row × MCUS-per-col.
  static bool jpegFileToBmpStreamPreview(FsFile& jpegFile, Print& bmpOut,
                                         const std::function<bool()>& shouldAbort = nullptr);

  // Progressive-stage decoder: produces a BMP at the requested fraction of
  // the final target dimensions.  Used by the BG worker's 5-stage preview
  // chain (placeholder → preview → low → mid → high → full): each stage
  // hits the next-coarsest hardware scale that JPEGDEC can do without
  // software upscaling, so the image gets visibly sharper every ~1-3 s.
  //
  // numerator/denominator define the fraction of `targetMaxWidth × Height`
  // to use as the scaled-fit box.  Examples:
  //   1/4 → "low"  stage (~113w of a 452w final target, ~2 s)
  //   1/2 → "mid"  stage (~226w, ~3 s)        — same as jpegFileToBmpStreamMid
  //   3/4 → "high" stage (~339w, ~5 s)
  static bool jpegFileToBmpStreamFraction(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                          int numerator, int denominator,
                                          const std::function<bool()>& shouldAbort = nullptr);

  // Convenience wrappers used by the FB2 BG worker — each just calls
  // jpegFileToBmpStreamFraction with the matching numerator/denominator so
  // the call sites in decodePendingImages read like an ordered ladder.
  static bool jpegFileToBmpStreamLow(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                     const std::function<bool()>& shouldAbort = nullptr);
  static bool jpegFileToBmpStreamMid(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                     const std::function<bool()>& shouldAbort = nullptr);
  static bool jpegFileToBmpStreamHigh(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                      const std::function<bool()>& shouldAbort = nullptr);

  // Generic stream-source decoder.  Takes JPEGDEC's own callback ABI
  // directly — the caller supplies pfnRead/pfnSeek/pfnClose plus a void*
  // handle that's passed to them.  Used by Fb2::decodeImageDirect to feed
  // a Base64JpegPump (no intermediate JPEG file on SD).  Output goes to
  // `bmpOut` as a 1-bit BMP, identical format to jpegFileToBmpStreamWithSize.
  //
  // `fileSize` is the upper bound passed to JPEGDEC::open() — it's the
  // estimated decoded JPEG byte length, not the source byte length.
  static bool jpegStreamToBmp(JPEG_READ_CALLBACK pfnRead, JPEG_SEEK_CALLBACK pfnSeek,
                              JPEG_CLOSE_CALLBACK pfnClose, void* fHandle, int32_t fileSize,
                              Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                              const std::function<bool()>& shouldAbort = nullptr);

  // -------------------------------------------------------------------------
  // Persistent decode-arena lending (v2.0.59)
  //
  // The JPEG decoder keeps a single contiguous scratch arena alive for the
  // whole session — see DecodeArena comment in JpegToBmpConverter.cpp.  Since
  // v2.0.59 we size the arena to also fit the resulting BMP file (header +
  // 1bpp pixel data) so the BMP-render step (ImageBlock::render slurping
  // /img/<book>/<id>.bmp from LittleFS) can read into the same arena
  // instead of allocating its own contiguous 26-60 KB chunk.  That
  // allocation routinely fails on a fragmented heap even when total free
  // is plentiful — the arena bypasses fragmentation completely because
  // it lives at a stable address from first-decode onward.
  //
  // Returns nullptr if the arena hasn't been allocated yet (no JPEG decoded
  // so far this session) OR `needed` exceeds the arena's current capacity.
  // Caller must NOT hold the pointer past the immediate operation — the
  // next JPEG decode could grow/move the buffer.  No locking: the assumption
  // is that BG-worker decode and UI BMP-slurp don't overlap in practice
  // (the BG worker yields before each render cycle).  If overlap happens
  // the worst case is a corrupted BMP read → placeholder fallback, never
  // a heap-corruption crash.
  static uint8_t* borrowDecodeArena(size_t needed);
  static size_t decodeArenaCapacity();

  // -------------------------------------------------------------------------
  // Startup heap warmup (v2.0.63)
  //
  // Force-allocate the persistent JPEGDEC instance (~25 KB) and the decode
  // arena (default 32 KB) NOW, before FreeRTOS tasks and the reader pipeline
  // scatter small allocations across the heap.  Calling this from setup()
  // pins both large blocks at one end of the heap; future small allocations
  // distribute in the remaining contiguous region instead of fragmenting
  // whatever's left around lazily-allocated big blocks mid-session.
  //
  // Without warmup the largest free heap block converges to ~7.7 KB after a
  // few minutes of reading, blocking BG cache extends.  With warmup the
  // largest block stays in the 25-40 KB range.
  //
  // Pass a smaller arenaBytes if memory is tight; the arena will still grow
  // on demand.  Pass 0 to skip the arena (only allocates JPEGDEC).
  static void warmup(size_t arenaBytes = 32 * 1024);
};
