# SmolJpeg — one-pass 1-bit JPEG decoder

Port of `smol-epub/jpeg.rs`.  Decodes baseline DCT JPEG → 1-bit Floyd-Steinberg
dithered BMP in one pass, no intermediate full-image grayscale buffer.

## Status (v2.0.88)

**INTEGRATED** in the `v3_alpha` env under `SNAPIX_SMOL_JPEG=1`.
`ImageConverter::convertJpegSd` / `convertJpegLittleFs` try SmolJpeg first,
fall back to picojpeg on any decode failure.

## What it supports

- Baseline DCT (SOF0)
- 1-4 component frames (YCbCr, K)
- 4:4:4 / 4:2:2 / 4:2:0 / 4:1:1 chroma subsampling — chrominance is
  Huffman-decoded then discarded
- 8-bit precision (the only kind shipped in EPUB cover images)
- Restart markers (RSTn) honoured
- Source pixel cap: 2048 × 2048

## What it doesn't support

- Progressive scans (SOF2) — returns `Status::ProgressiveScanLimit`
- 12-bit / lossless / hierarchical modes
- Exif rotation metadata (cover images rarely need it)

## Public API

```cpp
namespace snapix::smoljpeg {
class InputStream;     // random-access byte source
class OutputStream;    // sequential byte sink
Status decodeTo1BitBmp(InputStream& in, OutputStream& out, int maxW, int maxH,
                       bool (*shouldAbort)() = nullptr);
Status peekDimensions(InputStream& in, uint16_t& w, uint16_t& h);
}
```

## Footprint

- ~7.6 KB code, ~250 B rodata
- Heap-transient during decode: ~25 KB (JpegState + MCU-row Y buffer +
  dither error rows + zlib dict ring not needed here)
- BSS: 0

## Hardware testing notes

When you flash `v3_alpha`, the Serial log will show
`SmolJpeg(SD) decode failed (xxx); falling back to picojpeg` if SmolJpeg
chokes on something — that's the canary.  Things to watch for:

- Greyscale-only JPEGs (component count == 1)
- Restart-marker-heavy JPEGs (very compressed images, often photographic)
- Edge cases with non-MCU-aligned dimensions
