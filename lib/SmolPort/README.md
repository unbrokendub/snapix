# SmolPort — Snapix v3.0 refactor foundation

> *Phase 1 of 7.  Foundation only — no functional behaviour change in this
> release.  See `platformio.ini` `[env:v3_alpha]` for the rolling
> integration of the next phases.*

## Why this exists

Snapix v2.x is heap-malloc-heavy: every page parse allocates dozens of
`Page` / `PageElement` / `TextBlock` / `ParsedText` instances; every ZIP
inflate / XML parse / image decode malloc's its own scratch buffers.
That worked, but cost us ~30 versions worth of v2.0.x reliability
fixes for heap fragmentation, OOMs, JPEGDEC arena pinning, etc.

In late 2026 three independent Rust e-readers for the same hardware
(Xteink X4 / ESP32-C3) converged on a fundamentally different
architecture:

1. **Byte-marker styled text** — instead of a tree of typed page
   elements, a chapter is a flat byte stream of UTF-8 text with inline
   `[0x01, tag]` escape sequences for style/structure (bold, italic,
   headings, page breaks, image refs, anchors).  See `Markers.h`.

2. **Static workspace in BSS** — every parser buffer (ZIP central dir
   cache, inflate state, XML parse, catalog, path buf) lives in a
   single named struct allocated `static` at compile time.  Zero
   malloc during chapter parse; heap fragmentation becomes impossible
   by construction.

3. **Integrated 1-bit image decoders** — JPEG / PNG decoders that
   produce the dithered 1-bit BMP directly during decode, skipping
   the 8-bit grayscale intermediate buffer entirely.  ~20-30 KB less
   peak RAM per image vs the v2.x JPEGDEC + separate-pass approach.

4. **Unified per-book .bin cache** — single file per book containing
   header + chapter offsets + concatenated chapter text + image index +
   image data.  Replaces v2.x's dozens of `sections/N.bin`,
   `chapters/N.src.html`, `images/<hash>.bmp` files.

The reference implementations are:

* **hansmrtn/smol-epub** — the library-form crystallisation of all
  four patterns above.  MIT-OR-Apache-2.0.  ~8,100 lines of Rust.
* **HookedBehemoth/TrustyReader** — full firmware fork using
  smol-epub patterns.  Demonstrates the design in production shape.
* **ioma8/cool** — alternative full firmware that's even stricter on
  the no-heap constraint (everything in `static mut MaybeUninit`).

SmolPort ports the patterns — not the code — to C++ inside Snapix.
The Rust code can't be linked directly (we're on Arduino-ESP32, not
esp-hal), but the *design* is what saves heap and improves latency.
Each phase below stands alone: it can ship to users while later
phases are still under development.

## Phase plan

| # | Phase                                                      | Status | LOC est.  |
|---|------------------------------------------------------------|--------|-----------|
| 1 | Foundation: markers + stream reader + build flags          | **DONE** (v2.0.87) | ~300 |
| 2 | Integrated 1-bit JPEG decoder (port `smol-epub/jpeg.rs`)   | pending | ~1500 |
| 3 | Byte-marker HTML stripper (port `smol-epub/html_strip.rs`) | pending | ~2000 |
| 4 | Static `EpubRenderWorkspace` in BSS                        | pending | ~500  |
| 5 | Unified single-`.bin` cache format                          | pending | ~1000 |
| 6 | Integrated 1-bit PNG decoder (port `smol-epub/png.rs`)    | pending | ~800  |
| 7 | Adapt FB2 parser to byte-marker output                     | pending | ~400  |

Each phase lands behind a `SNAPIX_*` compile flag (see
`platformio.ini`).  The default env keeps every flag at `0` until the
phase is proven on real hardware against the test corpus.

## Build flags

```
SNAPIX_SMOL_JPEG       — Phase 2 — use SmolPort JPEG decoder in ImageConverter
SNAPIX_SMOL_HTML       — Phase 3 — use byte-marker HTML stripper in EpubChapterParser
SNAPIX_STATIC_WORKSPACE — Phase 4 — replace per-extract malloc with BSS workspace
SNAPIX_UNIFIED_CACHE   — Phase 5 — single .bin per book instead of per-spine files
SNAPIX_SMOL_PNG        — Phase 6 — use SmolPort PNG decoder in ImageConverter
```

Two envs ship in `platformio.ini`:

* `[env:default]` — every flag at 0.  Equivalent to v2.0.86 + version bump.
* `[env:v3_alpha]` — flags flip on as each phase lands.  Use for hardware
  A/B testing against the same book / same firmware build.

## License

SmolPort itself is under the same license as Snapix.  Ported algorithms
trace to `hansmrtn/smol-epub` (MIT-OR-Apache-2.0); attribution comments
on each ported module name the upstream file the code was derived from.

## Marker protocol

See `Markers.h` for the full byte-encoding spec.  Quick reference:

```
  [text bytes (UTF-8)] ...
  [0x01, 'B']                                    bold on
  [0x01, 'b']                                    bold off
  [0x01, 'I']                                    italic on
  [0x01, 'i']                                    italic off
  [0x01, 'H', level_digit ('1'..'6')]            heading on (with level)
  [0x01, 'h']                                    heading off
  [0x01, 'Q']                                    quote on
  [0x01, 'q']                                    quote off
  [0x01, 'L']                                    line break (<br>)
  [0x01, 'P']                                    paragraph break
  [0x01, 'N']                                    page break (forced)
  [0x01, 'S']                                    thematic break (<hr>)
  [0x01, 'M', len_lo, len_hi, path_bytes...]    inline image reference
  [0x01, 'A', len_lo, len_hi, anchor_bytes...]  anchor / hyperlink target
  [0x01, 0x01]                                  literal U+0001 in text
```

`MarkerStreamReader` walks the stream byte-by-byte and fires events on a
`MarkerObserver` subclass; see `MarkerStream.h` for the API.

## v2.0.88 test coverage

- **HtmlStripper** — 37 tests
- **Fb2Stripper** — 18 tests + 37 HTML regression checks
- **MarkerStreamReader** — 18 tests.  v2.0.88 fix: `kHeadingOn` dispatch
  routed through `InPayloadLenLo` and corrupted `payloadLen_` — now
  jumps directly to `InPayloadBody`.  No consumers in v2.x, so no
  firmware behaviour change; the fix matters when Phase 3e starts
  emitting heading markers downstream.
- **Round-trip (HtmlStripper ↔ MarkerStreamReader)** — 12 integration
  tests verifying the writer side's byte stream is byte-for-byte
  readable by the reader.  Catches protocol regressions across either
  lib.

## Phase 3 integration sketch

The remaining step is wiring HtmlStripper / Fb2Stripper output into the
existing chapter-rendering pipeline.  Today's `ChapterHtmlSlimParser`
(1776 LOC) takes Expat callbacks and emits `Page` objects with pre-
laid-out `WordData`.  Two migration paths:

1. **Adapter shim (lower risk).**  A `MarkerStreamToExpat` observer
   consumes `MarkerStreamReader` events and re-emits Expat-style
   `startElement` / `endElement` / `characterData` calls into the
   existing parser.  Layout engine untouched.
2. **Full replacement (higher risk).**  Rewrite the layout-emission
   half of `ChapterHtmlSlimParser` to consume `MarkerObserver` events
   directly.  Smaller binary, but the parser surgery wants
   hardware-in-loop verification before commit.

Sketch (under `SNAPIX_SMOL_HTML=1`):

```cpp
#include <Fb2Stripper.h>
#include <HtmlStripper.h>
#include <MarkerStream.h>

class MyLayoutObserver : public snapix::smolport::MarkerObserver {
  // Override onText / onBoldStart / onAnchor / onImageRef / etc.
  // Translate to your existing Page / PageElement build pattern.
};

MyLayoutObserver layout;
snapix::smolport::MarkerStreamReader reader(layout);

class ReaderSink : public snapix::smolport::HtmlStripperSink {
  snapix::smolport::MarkerStreamReader& r_;
 public:
  explicit ReaderSink(snapix::smolport::MarkerStreamReader& r) : r_(r) {}
  void emit(const uint8_t* d, size_t n) override { r_.feed(d, n); }
};

ReaderSink sink(reader);
// Use HtmlStripper for XHTML, Fb2Stripper for FB2:
snapix::smolport::HtmlStripper stripper(sink);
while (more_html_chunks) stripper.feed(chunk, len);
stripper.finish();
reader.finish();
```
