# SmolPng — one-pass 1-bit PNG decoder

Port of `smol-epub/png.rs`.  Mirrors SmolJpeg's design — chunk parse +
uzlib deflate + scanline filter inversion + RGB→luma + Floyd-Steinberg
dither, all streaming.

## Status (v2.0.88)

**INTEGRATED** in the `v3_alpha` env under `SNAPIX_SMOL_PNG=1`.
`ImageConverter::convertPngSd` / `convertPngLittleFs` try SmolPng first
when `oneBit` is requested, fall back to pngle on any failure.

## What it supports (MVP)

- Color type 2 (RGB, 8-bit) — common EPUB cover format
- Color type 6 (RGBA, 8-bit) — alpha blended against white background
- Non-interlaced only
- Filter method 0 (None / Sub / Up / Average / Paeth)
- Multi-IDAT chunked streams (concatenated transparently)
- Source pixel cap: 2048 × 2048

## What it doesn't support yet

- Color type 0 (grayscale) — would be easy to add, just luma = byte
- Color type 3 (palette) — needs PLTE + tRNS handling
- Color type 4 (gray+alpha) — same trivial change as 0 + 6
- 1/2/4/16-bit depth
- Adam7 interlacing

Anything unsupported returns `Status::UnsupportedFeature` and triggers
the pngle fallback.

## Public API

Same shape as SmolJpeg (intentional symmetry):

```cpp
namespace snapix::smolpng {
class InputStream;
class OutputStream;
Status decodeTo1BitBmp(InputStream& in, OutputStream& out, int maxW, int maxH,
                       bool (*shouldAbort)() = nullptr);
Status peekDimensions(InputStream& in, uint16_t& w, uint16_t& h);
}
```

## Footprint

- ~3 KB code (parser + decoder + filter + dither + BMP writer)
- ~290 B rodata
- Heap-transient during decode: ~50 KB (32 KB uzlib dict ring + 2 row
  buffers + dither errors + luma)
- BSS: 0

## Hardware testing notes

Same fallback log pattern as SmolJpeg.  Watch for:

- Palette PNGs (common in book covers from older publishers)
- Grayscale PNGs (some EPUBs convert covers to grayscale before
  embedding)
- Interlaced PNGs (rare but exist)

All three trigger pngle fallback — verify the fallback log appears
rather than a crash.

## uzlib dependency

SmolPng depends on `lib/uzlib/` for deflate decompression.  Skips the
2-byte zlib header manually (mirrors the existing `InflateReader`
convention in snapix — no `uzlib_zlib_parse_header` symbol shipped).
