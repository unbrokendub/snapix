#pragma once

// =============================================================================
// StreamingPaginator — event-driven page layout for byte-marker text streams.
//
// PROBLEM SOLVED:
//   Snapix v2.x builds a heap-allocated tree of `Page` → `PageElement` →
//   `TextBlock` → `WordEntry` per chapter parse.  Hundreds of small heap
//   allocations cumulatively fragment the ESP32-C3's ~145 KB heap.  After
//   ~30 versions of fixes (BSS workspaces, spill files, JPEG reuse,
//   chunk=1 fallback) the architectural ceiling is reached: ~7 KB largest-
//   free-block under load, cold rebuilds frequently deferred.
//
// THE SHIFT (Phase R1 of the v3 architectural refactor):
//   Three Rust e-readers for the same hardware (smol-epub, TrustyReader,
//   ioma8/cool) converged on a stream-of-events architecture: parser
//   emits marker events, paginator consumes events and decides where
//   pages break, renderer draws directly to framebuffer.  No tree, no
//   per-page allocations — fixed-size BSS workspace handles everything.
//
//   See `lib/SmolPort/README.md` for the full plan; this file is the
//   landing pad for the layout half of that architecture.  The marker
//   protocol + reader (`Markers.h`, `MarkerStream.h`) are already in
//   tree from v2.0.87.
//
// LIFECYCLE:
//   1. Construct once with `StreamingPaginatorConfig` (page geometry,
//      line heights) and a `PaginatorRenderer` (font/measure/draw
//      callbacks — abstracts GfxRenderer for testability).
//   2. Per chapter: call `resetForChapter()`.
//   3. Per page: call `resetForNextPage()`, then feed marker events
//      via the `MarkerObserver` interface.  Stop feeding when
//      `isPageFull()` returns true.
//   4. After each page: read `cursorY()` for status / progress, save
//      the byte-offset where the page ended (caller's responsibility —
//      provided by the marker stream reader's position).
//
// MEMORY MODEL (the whole point):
//   * Cursor + style state: ~16 bytes.
//   * Per-line word buffer: 256 B text + 32×8 B word slots = ~512 B.
//   * Total instance: ~528 B.  Suitable for stack OR BSS.  Zero heap
//     allocations during page layout.
//
// SCOPE OF THIS FILE (Phase R1 — skeleton, ships dark):
//   * Class structure + observer dispatch.
//   * Cursor advancement on line / paragraph / page events.
//   * Per-line word accumulation with greedy line breaker.
//   * Page-full detection.
//
// NOT YET (later R-phases):
//   * Real glyph-width measurement (today: stubbed via PaginatorRenderer).
//   * Full integration with GfxRenderer (Phase R3).
//   * Marker → markers-file caching (Phase R2).
//   * Image / anchor handling.
//   * Hyphenation, RTL, Thai, Arabic — these need their own integration
//     passes after the basic text path is proven on hardware.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <memory>  // v2.0.151 — heap-allocated spilloverBuf_

#include "MarkerStream.h"

namespace snapix::smolport {

// Style bit flags — packed into uint8_t per-word and as paginator state.
// Caller can extend; renderer must agree on the encoding.
constexpr uint8_t kStyleBold     = 1 << 0;
constexpr uint8_t kStyleItalic   = 1 << 1;
constexpr uint8_t kStyleHeading  = 1 << 2;
constexpr uint8_t kStyleQuote    = 1 << 3;

// Geometry + style metrics for a single render pass.  All sizes in
// logical pixels (post-orientation transform).  Lives on the caller's
// stack or in a config struct — paginator only reads.
struct StreamingPaginatorConfig {
  uint16_t pageWidth;        // e.g. 480 for portrait Xteink X4
  uint16_t pageHeight;       // e.g. 800
  uint16_t marginTop;        // top of content area
  uint16_t marginBottom;     // bottom of content area
  uint16_t marginLeft;
  uint16_t marginRight;
  uint16_t bodyLineHeight;   // pixels per line for body text
  uint16_t headingLineHeight;// pixels per line for heading text
  uint16_t paragraphSpacing; // extra pixels added between paragraphs
  // v3.1.0 — "красная строка": pixels the FIRST line of each top-level prose
  // paragraph is indented (0 = off / web-style flush-left).  Applied only to
  // the first line after a paragraph/thematic/section break — not to soft
  // wraps, <br>, verse lines, headings, centered text, or list/quote blocks
  // (those already carry their own indentation).  Classic book typography.
  uint16_t firstLineIndent;

  // v3.3.0 — word hyphenation.  When `hyphenate` is true the paginator splits
  // an overflowing word at a dictionary break point, drawing "pre-" on the
  // current line and the remainder on the next, instead of leaving a ragged
  // gap (Liang patterns via the Hyphenation lib; `hyphenLang` selects the
  // dictionary, e.g. "ru"/"en"/"de").  The paginator calls
  // Hyphenation::setLanguage(hyphenLang) itself in resetForChapter so the
  // MEASURE walk and the DRAW pass always use the SAME dictionary (the global
  // can't drift between them).  Hyphenation NEVER splits across a page
  // boundary — see the gate in tryAddWord — so .idx byte offsets (which are
  // always whole-word) are unaffected; only intra-page line packing changes.
  // Default false (zero-init) keeps the old ragged-justify layout for tests.
  bool hyphenate;
  char hyphenLang[8];
};

// Renderer abstraction — paginator delegates measurement + drawing here.
// Concrete subclasses adapt to:
//   * GfxRenderer (production — Phase R3 integration)
//   * Recording observer (unit tests — captures emitted lines for asserts)
//   * No-op (MEASURE-only mode for cache build)
//
// All callbacks must be cheap and side-effect-free in MEASURE mode.
class PaginatorRenderer {
 public:
  virtual ~PaginatorRenderer() = default;

  // Measure the pixel width of a UTF-8 byte run rendered with `styleBits`.
  // Called ONCE per word — paginator caches the result in the line buffer.
  virtual uint16_t measureWidth(const uint8_t* text, size_t len, uint8_t styleBits) = 0;

  // Pixel width of a single space character with `styleBits`.  Hoisted
  // because spaces dominate inter-word gaps and we measure many per page.
  virtual uint16_t getSpaceWidth(uint8_t styleBits) = 0;

  // Draw a single word at the given baseline coordinates with `styleBits`.
  // No-op in MEASURE mode.  Called per word (the paginator handles word
  // separation and line wrapping; the renderer just paints).
  virtual void drawWord(uint16_t x, uint16_t y, const uint8_t* text, size_t len,
                        uint8_t styleBits) = 0;

  // v2.0.145 — Resolve an image-ref `path` to its on-screen pixel
  // dimensions (post-scaling for the target screen).  Returns true on
  // success and fills `outWidth` / `outHeight`; returns false if the
  // image is missing or undecodable.
  //
  // Default implementation returns false so existing renderers (unit-test
  // FakeRenderer) that don't care about images continue to work; the
  // device's GfxRendererPaginatorAdapter overrides to consult its image
  // cache.
  virtual bool resolveImageSize(const uint8_t* /*path*/, size_t /*len*/,
                                  uint16_t* /*outWidth*/,
                                  uint16_t* /*outHeight*/) {
    return false;
  }

  // v2.0.145 — Draw a previously-resolved image at `x, y` with
  // `width × height` pixel dimensions.  Caller has already advanced
  // cursor past the image area; this just paints.  No-op in MEASURE
  // mode.  Default no-op for renderers that don't support images.
  virtual void drawImage(uint16_t /*x*/, uint16_t /*y*/,
                          uint16_t /*width*/, uint16_t /*height*/,
                          const uint8_t* /*path*/, size_t /*len*/) {}
};

// =============================================================================
// StreamingPaginator — implements MarkerObserver.
//
// Drives one page of layout per "fill cycle" between resetForNextPage() and
// isPageFull() going true.  Stateless across chapters except via the
// resetForChapter() entry point.
// =============================================================================
class StreamingPaginator : public MarkerObserver {
 public:
  // Capacity caps for the per-line scratch buffers.  Sized for a typical
  // body line on Xteink X4 (480-px wide, ~10-15 words at average width).
  // Overflow falls back to an early line flush — no heap escape.
  static constexpr size_t kLineTextCapacity = 256;  // bytes
  static constexpr size_t kLineWordCapacity = 32;   // word slots

  // v2.0.145 — indent step in pixels, applied per kIndentOn level.
  // Roughly one body line height — large enough to be visually distinct,
  // small enough that 4 levels still leave usable line width on a 480-px
  // screen (= max 96 px indent at 24-px step).
  static constexpr uint16_t kIndentStep = 24;
  static constexpr uint8_t  kMaxIndentDepth = 4;

  StreamingPaginator(const StreamingPaginatorConfig& cfg, PaginatorRenderer& renderer);

  // ----- MarkerObserver overrides (event consumers) -----
  ObserverStatus onText(const uint8_t* text, size_t len) override;
  ObserverStatus onLineBreak() override;
  ObserverStatus onParagraphBreak() override;
  ObserverStatus onPageBreak() override;
  ObserverStatus onThematicBreak() override;
  ObserverStatus onHeadingStart(uint8_t level) override;
  ObserverStatus onHeadingEnd() override;
  ObserverStatus onBoldStart() override;
  ObserverStatus onBoldEnd() override;
  ObserverStatus onItalicStart() override;
  ObserverStatus onItalicEnd() override;
  ObserverStatus onQuoteStart() override;
  ObserverStatus onQuoteEnd() override;
  ObserverStatus onIndentStart() override;
  ObserverStatus onIndentEnd() override;
  ObserverStatus onCenterStart() override;
  ObserverStatus onCenterEnd() override;
  ObserverStatus onImageRef(const uint8_t* path, uint16_t len) override;
  ObserverStatus onStreamEnd() override;

  // ----- caller queries -----
  // True once cursorY would advance past `marginBottom`.  Caller stops
  // feeding when this returns true.  (Already-flushed words are drawn;
  // the word that triggered overflow is buffered for the next page.)
  bool isPageFull() const { return pageFull_; }

  // v2.0.121 Phase R3.4 — MEASURE/DRAW gate.  When `true`, drawWord()
  // calls into the renderer are SUPPRESSED but page-fullness logic
  // (line wrapping, vertical overflow detection) still runs as
  // normal.  Used to fast-skip the first N-1 pages when rendering
  // page N: paginator advances through the stream computing line
  // breaks and detecting page boundaries, but no pixels are drawn
  // until the caller flips the flag back to false on the target
  // page.  Toggleable mid-stream.
  void setDrawSuppressed(bool suppressed) { drawSuppressed_ = suppressed; }
  bool isDrawSuppressed() const { return drawSuppressed_; }

  // v2.0.125 R3.6 — direct setter for active style bits.  Used by
  // `renderMarkerizedPage`'s resume path to restore paginator
  // style state at a known page boundary, so a bold/italic span
  // open across the resume point continues rendering correctly
  // even though we skipped the events that opened it.
  void setStyleBits(uint8_t bits) { styleBits_ = bits; }
  uint8_t styleBits() const { return styleBits_; }

  uint16_t cursorX() const { return cursorX_; }
  uint16_t cursorY() const { return cursorY_; }

  // v2.0.140 — source-byte-offset accounting for accurate `.idx` byteOffsets.
  //
  // PROBLEM SOLVED:
  //   Pre-fix, `PageCountingObserver` derived byteOffset as
  //   `bytesSeen_ - pendingBytes()`.  This is correct when the carry word
  //   was set DURING the current text call (all relevant bytes are in
  //   carry+spillover).  But when a partial word buffered across a chunk
  //   boundary gets carried by a NON-TEXT event handler (line break /
  //   anchor / image / style toggle) calling `flushPartWord`, the event's
  //   marker bytes (2, 3 or 4 each) are already in `bytesSeen_` but NOT
  //   in pendingBytes() — there's no place for them.  Result: byteOffset
  //   points PAST the carry word's first byte by exactly the event-marker
  //   size, and resume seek lands mid-UTF-8-codepoint.  For Russian text
  //   with 2-byte chars: a 4-byte offset error (anchor with empty
  //   payload) lands 2 chars deep into the carry word, rendering the
  //   continuation byte as `?` followed by the next char.  User-visible
  //   as `?o написать` instead of `что написать` at every page boundary.
  //
  // DESIGN:
  //   Caller (PageCountingObserver) hands the paginator the source byte
  //   offset of the upcoming text call via `setCurrentSourceOffset(...)`.
  //   The paginator records the partial word's first-byte source offset
  //   when buffering, propagates it through extensions, and stores it
  //   into the carry slot when the partial becomes carry.  Caller reads
  //   `carryStartByteOffset()` after pageFull to get the accurate
  //   byteOffset for `.idx`.
  //
  //   No external bytes-counting math is needed in the observer once the
  //   paginator records absolute offsets internally.
  void setCurrentSourceOffset(uint32_t offset) { currentSourceOffset_ = offset; }

  // Source byte offset of the carry word's FIRST byte in the markers
  // stream.  Valid only while `carryWordLen() > 0`.  Set when the carry
  // is captured (either from the in-flight text path or from
  // `flushPartWord` triggered by a non-text event).
  uint32_t carryStartByteOffset() const { return carryStartByteOffset_; }

  // Length of the carry word in bytes — 0 means no carry pending.
  // Useful in `PageCountingObserver` to decide whether to use
  // `carryStartByteOffset()` or fall back to the bytesSeen-pending math
  // (for the "page filled by cursor advance, no carry word" case).
  uint16_t carryWordLen() const { return carryWordLen_; }

  // Accessor for the partial-word buffer size.  Used by external observers
  // (PageCountingObserver) to detect state transitions across event
  // boundaries.  Exposed publicly only for diagnostic / accounting use;
  // the partial bytes themselves are private.
  uint16_t partialWordLen() const { return partWordLen_; }

  // v2.0.137 — count of bytes the paginator has READ from `onText`
  // calls but not yet COMMITTED to a drawn slot.  Includes:
  //   * carryWordLen_   — the word that triggered pageFull
  //   * spilloverLen_   — bytes after the carried word in the same
  //                       onText that we couldn't deliver
  //                       (because we already returned Stop)
  // Excludes:
  //   * partWordLen_    — those bytes belong to the CURRENT page (a
  //                       word being assembled across chunks); they
  //                       eventually get committed via flushPartWord
  //                       or onStreamEnd, on the same page.
  //
  // Used by `PageCountingObserver` to compute the byteOffset for
  // `.idx` — `bytesSeen_ - pendingBytes()` is where the next page's
  // content actually begins in the source markers stream.  Resume
  // seek-to-byteOffset re-feeds those bytes from disk; the paginator
  // (which is freshly constructed on resume, with empty spillover
  // and carry) processes them as the start of the next page.
  uint16_t pendingBytes() const {
    return static_cast<uint16_t>(carryWordLen_ + spilloverLen_);
  }

  // Number of words on the current (unflushed) line — useful for tests
  // and for "how much will be carried over to the next page" decisions.
  uint16_t pendingWordCount() const { return lineWordCount_; }

  // ----- lifecycle -----
  // Begin layout of the next page.  Resets cursor to top of content area
  // and clears page-full flag.  PRESERVES style state (bold/italic toggles
  // continue across page breaks within a paragraph).
  void resetForNextPage();

  // Full reset — call when starting a new chapter.  Resets cursor AND
  // style state.  Equivalent to construction.
  void resetForChapter();

 private:
  // Single word entry in the per-line buffer.
  struct WordSlot {
    uint16_t textOffset;  // index into lineText_ where this word's bytes start
    uint16_t textLen;     // byte length (UTF-8)
    uint16_t pixelWidth;  // measured advance width
    uint8_t  styleBits;   // style at the time of capture
    // v2.0.135: when true, `flushLine` draws this slot DIRECTLY after the
    // previous slot's last pixel without inserting `spaceWidth` between
    // them.  Used for inline style toggles mid-word (HTML `хо<b>ро</b>шо`
    // becomes three slots — "хо" "ро" "шо" — that visually MUST glue
    // together since source had no whitespace between them).  Pre-fix
    // (v2.0.133-134), every slot got an inter-word space inserted in
    // flushLine, producing visible "хо ро шо" with two spurious spaces.
    // For Russian books with frequent inline emphasis this cumulatively
    // made lines wrap at half the screen width.  See `partWordContinuation_`
    // for the mechanism that propagates this flag from style-change
    // event handlers down to the next-submitted slot.
    bool     noLeadingSpace;
  };

  const StreamingPaginatorConfig& cfg_;
  PaginatorRenderer& renderer_;

  // Cursor + page state
  uint16_t cursorX_;
  uint16_t cursorY_;
  bool     pageFull_;
  bool     drawSuppressed_ = false;  // R3.4: MEASURE/DRAW gate, see setter

  // Whether the current line has any whitespace-stripped output yet
  // (used to decide whether to drop leading whitespace mid-paragraph).
  bool     lineHasContent_;

  // v3.1.0 — "красная строка" state.  True while the NEXT word to be placed
  // begins a fresh prose paragraph (set on chapter start + paragraph /
  // thematic / hard-page breaks; NOT on soft wraps or <br>/verse breaks).
  // Consumed (and cleared) by tryAddWord when it places the first word of a
  // line: if cfg_.firstLineIndent>0 and the context is a top-level body
  // paragraph (not centered/heading/indented), the line start is pushed in
  // by firstLineIndent.  Deliberately preserved across resetForNextPage so a
  // paragraph that broke exactly at the page boundary still indents its
  // continuation page's first line, while a mid-paragraph carry (flag false)
  // does not.
  bool     atParagraphStart_ = false;

  // v3.1.0 — extra left offset applied to the CURRENT line being built (the
  // "красная строка" first-line indent, or 0).  Set by tryAddWord when it
  // places the first word of a paragraph's first line; consumed by flushLine
  // as the line's draw-start x offset and reset to 0 afterwards so wrapped
  // continuation lines render flush-left.  (cursorX_ also carries the indent
  // for fit-checks, but flushLine recomputes the draw x from effectiveLeft,
  // so the offset must be threaded explicitly.)
  uint16_t lineIndent_ = 0;

  // Active style stack (flattened into bits — nesting is the stripper's
  // responsibility).
  uint8_t  styleBits_;

  // v2.0.145 — Indent depth (0 = no indent, max 4).  Each kIndentOn
  // pushes one level, each kIndentOff pops one (clamped to 0).
  // Effective left margin = cfg_.marginLeft + indentDepth_ * kIndentStep,
  // applied per-line at flush time and per-fit-check in tryAddWord.
  uint8_t  indentDepth_ = 0;
  // v2.0.145 — Center mode flag.  Set by kCenterOn, cleared by
  // kCenterOff (or heading transitions).  When true, flushLine
  // center-aligns the line instead of left-aligning + justifying.
  bool     centered_ = false;

  // v2.0.145 — pending image to draw on the next line flush.  Set by
  // onImageRef when the image fits (height ≤ remaining page space) and
  // has been resolved to pixel dimensions by the renderer.  Drawn by
  // flushLine before the next text line proceeds.  Empty when len == 0.
  static constexpr size_t kImagePathCapacity = 128;
  uint8_t  pendingImagePath_[kImagePathCapacity];
  uint16_t pendingImagePathLen_ = 0;
  uint16_t pendingImageWidth_ = 0;
  uint16_t pendingImageHeight_ = 0;

  // Per-line text + word table.  Refilled per line, drawn + discarded.
  uint8_t  lineText_[kLineTextCapacity];
  uint16_t lineTextLen_;
  WordSlot lineWords_[kLineWordCapacity];
  uint16_t lineWordCount_;

  // Carry-over word: when a word doesn't fit on the current line, we
  // flush the line WITHOUT it and stash it here.  At the start of the
  // next line (next page if page-full), we re-emit it.  Empty when
  // textLen == 0.
  uint8_t  carryWordText_[64];   // most words fit easily; oversized → presplit
  uint16_t carryWordLen_;
  uint16_t carryWordWidth_;
  uint8_t  carryWordStyle_;

  // v2.0.133 — partial word buffer for cross-call assembly.
  //
  // PROBLEM:
  //   MarkerStream emits a text run in one or more `onText` invocations
  //   depending on chunk boundaries (LittleFS reads 4 KB at a time).
  //   Pre-fix, each `onText` call's bytes were split into words by
  //   whitespace independently.  If a chunk boundary fell INSIDE a
  //   word, the bytes before the boundary were submitted as one word
  //   and the bytes after as another.  For Russian (UTF-8 2-byte chars),
  //   a chunk boundary typically lands inside the multi-byte sequence
  //   of a character — the lead byte (0xD0/0xD1) is rendered as the
  //   start of one "word" and the continuation byte (0x80-0xBF) as the
  //   start of the next, both producing latin-1 fallback glyphs
  //   (ÿ°, ÿÿ, etc.).  User-visible result: text like "категория"
  //   becomes "пÿ°тегория" mid-paragraph.
  //
  // FIX:
  //   When the LAST byte of `onText`'s input is non-whitespace, the
  //   trailing word is INCOMPLETE — we buffer it here instead of
  //   submitting as a finished word.  The NEXT `onText` call checks
  //   `partWordLen_` first and prepends these bytes onto the
  //   continuation up to the first whitespace (or buffer-full).  Non-
  //   text events (line break, style change, EOS, page reset) call
  //   `flushPartWord()` first so the buffered word doesn't leak past
  //   its rightful style/line context.
  //
  // CAPACITY:
  //   128 bytes accommodates ~60-char Russian compound words or long
  //   hyphenated URLs.  On overflow we flush as-is (truncating a single
  //   pathologically long word) rather than discarding — caller's
  //   reader handles the rest as a new word on the next iteration.
  static constexpr size_t kPartWordCapacity = 128;
  uint8_t  partWordBuf_[kPartWordCapacity];
  uint16_t partWordLen_ = 0;

  // v2.0.135 — inline-style-toggle continuation flag.
  //
  // PROBLEM (bug introduced by v2.0.133's partial-word buffer):
  //   HTML like `хо<b>ро</b>шо` (style toggle MID-word) is stripped by
  //   `HtmlStripper` to three text runs: "хо" — kBoldOn — "ро" — kBoldOff
  //   — "шо".  No whitespace separates them in source.  Each text run
  //   gets buffered as a partial, then committed via `flushPartWord` from
  //   the inline-style event handler.  Each `flushPartWord → tryAddWord`
  //   creates a separate `WordSlot`.  `flushLine` walks the slots and
  //   inserts `spaceWidth` between every adjacent pair — producing the
  //   visible string "хо ро шо" instead of the intended "хорошо".
  //   Russian books with frequent emphasis ended up rendering with so
  //   many spurious spaces that lines wrapped at half-width.
  //
  // FIX:
  //   `flushPartWord(setContinuation=true)` raises this flag after
  //   submitting the partial; the NEXT `tryAddWord` consumes it and
  //   marks the new slot with `noLeadingSpace=true`.  `flushLine` then
  //   skips the inter-word space for slots flagged this way.
  //
  //   Cleared on:
  //     * Each `tryAddWord` after consumption (one-shot).
  //     * Start of `onText` if the first byte is whitespace (source
  //       had a normal word break).
  //     * `resetForChapter` / `resetForNextPage` / hard-break events.
  //
  //   Continuation case in `onText` (partial completed by arriving
  //   whitespace) submits via direct `tryAddWord` — NOT through
  //   `flushPartWord` — so the flag does not get raised (whitespace is
  //   the natural separator).
  bool partWordContinuation_ = false;

  // v2.0.137 — text spillover buffer.
  //
  // PROBLEM:
  //   `MarkerStream::feed` advances its byte cursor past an entire text
  //   run BEFORE calling `observer_.onText(...)`.  If the paginator
  //   returns `Stop` mid-text-run (because `tryAddWord` triggered
  //   `pageFull_`), the bytes AFTER the failing word — still inside the
  //   same `onText` call — are silently DROPPED.  The carry slot rescues
  //   only the single word that triggered `pageFull_`; everything after
  //   it in the source goes missing.  Visible as:
  //     * Pages don't connect (chunks of text disappear at every page
  //       boundary in the middle of a paragraph).
  //     * UTF-8 garbage at boundaries (multi-byte char split across
  //       the drop, lead byte processed, continuation bytes dropped).
  //     * Resume from `.idx` lands BEYOND the dropped bytes, never
  //       sees them either.
  //
  // FIX:
  //   When `tryAddWord` fails (page-full carry), save the bytes from
  //   AFTER the carried word to the end of the text run in
  //   `spilloverBuf_`.  When `pageFull_` is already set at the top of
  //   `onText`, save the ENTIRE incoming text run.
  //
  //   `resetForNextPage` processes spillover (after `emitCarryWord`)
  //   so it lands on the next page in source order: carry word first,
  //   then spillover continues from where the source did.
  //
  //   `PageCountingObserver::checkAdvance` subtracts `pendingBytes()`
  //   from `bytesSeen_` when firing `onBoundary_`, so the stored
  //   `byteOffset` points to where the carry word starts in source.
  //   Resume seek to that offset re-feeds the dropped bytes from
  //   disk — equivalent to the live walk's in-memory spillover.
  //
  //   Capacity 256 covers typical text-run tails after page-full
  //   (a page worth of carry-over is ~one paragraph's worth of bytes).
  //   Overflow truncates — same loss class as v2.0.136 but only on
  //   pathological super-long text runs, not on every boundary.
  // v2.0.142 — bumped 256 → 2048 to fix missing-text-between-pages on
  // long Russian paragraphs (see v2.0.142 history note).
  //
  // v2.0.151 — moved from inline array (stack) to heap.  The 2 KB
  // inline buffer combined with chunkBuf[4096] + 12 KB parent-chain
  // stack frame overflowed loopTask AND ReaderAsync; v2.0.149/150
  // bumped both task stacks +8 KB each, eating 16 KB of heap.  That
  // turned out to push EPUB chapter prepare (ZIP deflate + HTML
  // normalize, ~30 KB peak heap) over the "heap dangerously low"
  // threshold for «Гибкий ум»'s big spine-9 chapter — extraction
  // aborted on every retry, page never loaded.
  //
  // Heap-allocating spilloverBuf saves ~2 KB stack per StreamingPaginator
  // instance.  Allocation: one malloc per `resetForChapter()` (called
  // once per render call) and one free per dtor — cheap relative to the
  // render itself.  No fragmentation risk since the allocation is
  // released before the render returns.
  // v2.0.207 — kSpilloverCapacity is now the INITIAL allocation; the buffer
  // GROWS on demand (up to kSpilloverMaxCapacity) instead of truncating.
  //
  // Pre-fix, saveSpillover() silently dropped any bytes past 2048 in a
  // single page-fill's text-run tail.  A long paragraph (one marker-free
  // text run, delivered up to chunkBufSize=4096 bytes at a time) that fills
  // a page EARLY leaves >2048 bytes of tail → those words were DROPPED,
  // visible as "words missing between pages" mid-paragraph.  The v2.0.142
  // bump 256→2048 reduced the frequency but didn't eliminate it.  Growing
  // the heap buffer removes the data loss entirely.
  //
  // The 16-bit spilloverLen_ caps the buffer at 64 KB; kSpilloverMaxCapacity
  // (16 KB) keeps carryWordLen_+spilloverLen_ well within uint16 for
  // pendingBytes() and is ~8 pages of carried text — unreachable in
  // practice (steady-state spillover is <1 page).  Beyond it we truncate
  // (graceful, strictly better than the old 2 KB cap).
  static constexpr size_t kSpilloverCapacity = 2048;
  static constexpr size_t kSpilloverMaxCapacity = 16384;
  std::unique_ptr<uint8_t[]> spilloverBuf_;
  size_t   spilloverCapacity_ = 0;  // current allocation size of spilloverBuf_
  uint16_t spilloverLen_ = 0;
  // v2.0.141 — absolute source byte offset of spilloverBuf_[0].
  // When saveSpillover is called with `text + i, len - i`, the spillover
  // bytes' first byte is at source position `currentSourceOffset_ + i`.
  // resetForNextPage restores `currentSourceOffset_` to this value
  // before re-processing the spillover via processTextRun, so words
  // carved out of spillover that later trigger carry get the right
  // absolute source offsets — without this, all carry/partial offsets
  // produced during spillover replay are off by the position-in-onText
  // where spillover started (negative — they point EARLIER than the
  // actual word, so resume seeks too early and reads content that's
  // already on the previous page → duplicated words at page starts;
  // user-visible alongside the "missing word at boundary" if multi-
  // chunk text runs straddle the boundary).
  uint32_t spilloverSourceOffset_ = 0;

  // v2.0.140 — source-byte-offset accounting state.
  //
  // currentSourceOffset_:
  //   Source byte offset at the START of the most recent `onText` call
  //   (i.e., `bytesSeen` BEFORE the call's text bytes were added).
  //   Pushed in by `PageCountingObserver::onText` before delegating.
  //   The paginator uses it to compute absolute source offsets for
  //   words submitted from the main loop or buffered as a partial.
  //
  // partialStartByteOffset_:
  //   Source byte offset of partWordBuf_'s FIRST byte in the markers
  //   stream.  Set when `partWordLen_` transitions 0 → N from the
  //   main-loop's end-of-text-without-whitespace branch (or oversized-
  //   word branch).  PRESERVED across gobble-extension (same word, same
  //   first byte position).  Cleared when partial is consumed (via
  //   tryAddWord success OR carried via tryAddWord failure).
  //
  // carryStartByteOffset_:
  //   Source byte offset of carryWordText_'s FIRST byte.  Set whenever
  //   `carryWordLen_` transitions 0 → N inside `tryAddWord`'s page-
  //   full path.  Caller (PageCountingObserver) reads this via
  //   `carryStartByteOffset()` to compute the accurate `.idx`
  //   byteOffset for the next page.  Cleared in `emitCarryWord` after
  //   the carry is replayed on the new page.
  uint32_t currentSourceOffset_ = 0;
  uint32_t partialStartByteOffset_ = 0;
  uint32_t carryStartByteOffset_ = 0;

  // -------------------- helpers --------------------

  // Append one UTF-8 word (whitespace-trimmed) to the current line buffer.
  // Measures via renderer_, computes whether it fits with leading space.
  // Returns: true on accepted, false on rejected (line full or page full).
  // On line-full rejection: caller flushes line and retries on next line.
  // On page-full rejection: stashes word in carry slot, sets pageFull_.
  //
  // `srcOffset`: source byte offset of `word`'s first byte in the markers
  // stream.  Stored into `carryStartByteOffset_` on page-full carry; used
  // by `PageCountingObserver` to compute accurate `.idx` byteOffsets.
  // Default 0 keeps the function callable from sites that don't track
  // source offsets (unit tests using raw `feedText`).
  bool tryAddWord(const uint8_t* word, size_t len, uint32_t srcOffset = 0);

  // v3.3.0 — try to hyphenate an overflowing word: draw "pre-" on the current
  // line and let the remainder flow onto the next line.  Returns true if the
  // word was split + placed; false → caller falls back to whole-word wrap.
  // Only ever splits when BOTH pieces stay on the SAME page (the remainder
  // fits one next line), so .idx page boundaries remain whole-word.  See the
  // hyphenate fields in StreamingPaginatorConfig.
  bool tryHyphenate(const uint8_t* word, size_t len, uint32_t srcOffset, bool addLeadingSpace);
  // Shortest word (bytes) worth a dictionary lookup — skips "и"/"на"/etc.
  static constexpr size_t kMinHyphenateBytes = 10;
  // Longest word we attempt to hyphenate (bytes); longer falls back to wrap.
  static constexpr size_t kMaxHyphenateBytes = 80;

  // Flush accumulated line: draw words (via renderer_), advance cursorY by
  // the appropriate line height, reset line buffer.  Detects page overflow
  // AFTER drawing; the line that was drawn always lands within bounds
  // because we pre-checked before adding it.
  //
  // v2.0.142 — `justify` controls horizontal alignment of the drawn slots:
  //   * false (default — used by event handlers that flush at the end
  //     of a paragraph / explicit break / page boundary): left-align.
  //     Words are drawn with the natural inter-word `spaceWidth`; line
  //     ends raggedly wherever the last word fits.
  //   * true (used by `tryAddWord` Case B — line wrapped because next
  //     word didn't fit): full justification.  The "slack" between
  //     the natural line end and the right margin is distributed
  //     evenly across the inter-word gaps so the line reaches the
  //     right margin.  Headings (`styleBits_ & kStyleHeading`) and
  //     single-word lines are NEVER justified (silly visual result).
  //     Slots with `noLeadingSpace=true` (inline-style continuations
  //     from the same source word) do NOT receive extra space — the
  //     visual word stays glued together.
  void flushLine(bool justify = false);

  // Discard line buffer without drawing (e.g. when an explicit page break
  // arrives mid-line and we want a clean cut).
  void clearLine();

  // Re-add the carry-over word to the new line, if any.  Called at the
  // top of every line that follows a full-line flush.
  void emitCarryWord();

  // v2.0.133 — submit buffered partial word as a complete word.  Called
  // from non-text event handlers (line break, style change, EOS) so a
  // word being assembled across multiple `onText` calls doesn't leak
  // past a structural boundary.  Idempotent when buffer is empty.
  // Sets `pageFull_` if the resulting word doesn't fit and the carry
  // slot was already occupied (best-effort: a partial word at a page
  // boundary is rare).
  //
  // v2.0.135 — `setContinuation`: when `true` (default — call from any
  // event handler), raise `partWordContinuation_` after submission so
  // the next-submitted word's slot is marked `noLeadingSpace=true`.
  // Pass `false` from the onText continuation path (whitespace ended
  // the partial naturally — next word should have normal inter-word
  // spacing) and from carry/reset paths.
  void flushPartWord(bool setContinuation = true);

  // v2.0.137 — extracted from `onText` so it can be re-invoked from
  // `resetForNextPage` to process spillover bytes (text that couldn't
  // be delivered when pageFull fired mid-text-run).  Returns Stop if
  // pageFull triggers during the run; spillover bytes are saved in
  // `spilloverBuf_` for the next call.
  ObserverStatus processTextRun(const uint8_t* text, size_t len);

  // v2.0.137 — append bytes to `spilloverBuf_` for processing on the
  // next page.  Truncates on overflow (pathological text runs longer
  // than `kSpilloverCapacity` after a page-full event); the loss class
  // matches pre-v2.0.137 behaviour where the entire tail was dropped.
  // No-op on empty input.
  //
  // v2.0.141 — `srcOffset` is the ABSOLUTE source byte position of
  // `text[0]`.  Recorded as `spilloverSourceOffset_` on the first
  // save of a flush cycle so resetForNextPage can restore
  // `currentSourceOffset_` to the right value before re-processing
  // the spillover (otherwise any carry captured during spillover
  // replay records the wrong `carryStartByteOffset_`).
  void saveSpillover(const uint8_t* text, size_t len, uint32_t srcOffset);

  // Current effective line height based on style bits.
  uint16_t currentLineHeight() const;

  // Vertical room remaining on the current page (pixels).
  uint16_t verticalRemaining() const;

  // Helper: does the byte at `*p` look like ASCII whitespace?
  static bool isAsciiSpace(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
  }
};

}  // namespace snapix::smolport
