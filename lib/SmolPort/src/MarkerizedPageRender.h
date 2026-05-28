#pragma once

// =============================================================================
// MarkerizedPageRender — page-targeted rendering of a marker-byte stream.
//
// PURPOSE (Phase R3.4 of the v3 architectural refactor):
//   Glues MarkerStreamReader → StreamingPaginator → PaginatorRenderer
//   into a single "render page N of this chapter" call.  Caller supplies
//   a chunked source reader (typically a LittleFS handle) and the
//   target page index (0-based within the chapter).
//
// PAGE-TARGETING STRATEGY:
//   The marker stream is a flat sequence of events; page boundaries are
//   detected by the paginator (vertical overflow → isPageFull true) or
//   explicit `kPageBreak` markers.  To render page N:
//     1. Construct paginator with `setDrawSuppressed(true)` so word
//        emission is no-op.
//     2. Stream events; each time `isPageFull()` fires, increment the
//        page counter and call `resetForNextPage()`.
//     3. When page counter reaches N, flip `setDrawSuppressed(false)`.
//     4. Continue streaming until next `isPageFull()` — that's page N+1
//        starting; we're done.
//
// COST:
//   * Per-page seek cost is O(N) — pages 0..N-1 are streamed but not
//     drawn.  For typical 30-page chapters this is ~300 KB of byte
//     reads + paginator state advance, ~200-400 ms total on LittleFS.
//   * Real-page render cost is independent of N.
//   * Future R3.6 optimisation: write a per-page-offset index into the
//     markers file when markerize runs, so target-page seek is O(1).
//
// NO HEAP ALLOCATIONS:
//   The chunk buffer is caller-supplied stack memory.  StreamingPaginator
//   + StreamingChapterRenderer + PaginatorRenderer instances all live
//   on the caller's stack.
//
// SCOPE OF THIS FILE (Phase R3.4 — building block, ships dark):
//   * The render function itself.
//   * Tested host-side via FakePaginatorRenderer (no GfxRenderer
//     dependency).
//
// NOT YET (R3.5):
//   * Wire-up into `ReaderState::renderCachedPage` as opt-in path
//     behind `SNAPIX_MARKERIZED_RENDER` flag.
//   * Image / anchor handling — current implementation skips them
//     (paginator's default observer hooks are no-op for `onImageRef`
//     and `onAnchor`).
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "StreamingChapterRenderer.h"
#include "StreamingPaginator.h"

namespace snapix::smolport {

// Status of a `renderMarkerizedPage` call.  Success means the target
// page was reached AND drawn; PageNotFound means the stream ended
// before the page counter reached `targetPage`.
enum class MarkerizedRenderResult : uint8_t {
  Success,
  PageNotFound,
  ReadError,
  Aborted,
};

// Stats for diagnostics.
struct MarkerizedRenderStats {
  uint16_t pagesAdvancedThrough;  // pages skipped before target (=targetPage)
  uint32_t bytesConsumed;
  uint16_t chunksProcessed;
};

// v2.0.125 R3.6 — per-page resume snapshot.  Captured by the
// `onPageBoundary` callback during render; replayed via the
// `resume` parameter on subsequent renders to skip the
// MEASURE-only walk through earlier pages.
//
// `byteOffset` = total bytes consumed from `readChunk` at the
// moment the paginator's currentPage_ rolled to `pageIndex`.
// Caller may use it to seek the LittleFS file directly to that
// offset before calling renderMarkerizedPage with a non-zero
// `resume.startPage`.
//
// `styleBits` = paginator's `styleBits_` at that moment; carried
// forward so a bold/italic span open across the resume point
// continues correctly.
struct PageBoundarySnapshot {
  uint16_t pageIndex;
  uint32_t byteOffset;
  uint8_t  styleBits;
};

using OnPageBoundaryFn = std::function<void(const PageBoundarySnapshot&)>;

// Resume parameters — all default-zero means "render from scratch
// (fresh chapter)" and behaviour is identical to v2.0.121-122.
//
// When `startPage > 0`:
//   * Caller must have already seeked the source past the bytes
//     of pages 0..startPage-1 (i.e. `readChunk` returns bytes
//     belonging to page `startPage` and onward).
//   * `byteOffset` MUST equal the source byte position at which
//     `readChunk` starts returning bytes (i.e. the file-level seek
//     position).  Used by `PageCountingObserver` to compute
//     ABSOLUTE byteOffsets for the new page boundaries it captures
//     — without this, subsequent boundary callbacks fire with
//     offsets RELATIVE to the seek start, so the caller's `.idx`
//     cache accumulates wrong (relative) offsets every time a
//     resume render captures a new boundary.  On-device symptom
//     pre-fix: cumulative resume drift, "first 3 lines change
//     between pages" once the user navigates past the cold-walk
//     range, because every resume seeks slightly past the start
//     of the chapter instead of to the actual target page.
//   * Paginator initial currentPage_ is set to `startPage`, so it
//     thinks the target page is reached immediately when
//     `targetPage == startPage`.
//   * Paginator initial styleBits_ is set to `styleBits`, so any
//     style scope open at the resume point continues.
//
// Caller responsibility to keep the byteOffset/startPage/styleBits
// triple consistent; passing inconsistent values produces wrong
// layout (no crash, just cosmetic).
struct MarkerizedRenderResume {
  uint16_t startPage = 0;
  uint8_t  styleBits = 0;
  // v2.0.140 fix — absolute source byte position of the first byte
  // `readChunk` returns.  Default 0 means "fresh chapter, no seek"
  // and matches pre-fix behaviour for cold walks.  When resuming,
  // the caller MUST pass the same value it used for the file-level
  // seek (e.g., `mf.seek(byteOffset)` then `resume.byteOffset =
  // byteOffset`).  PageCountingObserver uses this as the initial
  // value of its internal bytesSeen counter so all stored
  // byteOffsets remain absolute (matching the cold-walk values
  // already in `.idx`).
  uint32_t byteOffset = 0;
};

// =============================================================================
// v2.0.128 R4.a — persistent page-offset index (`.idx` sidecar next to
// `.markers`).  Lets the streaming render path skip the MEASURE-only
// walk between book-open sessions by saving discovered page boundaries
// to disk.
//
// File format (little-endian throughout):
//   Header (12 bytes):
//     uint32_t magic           = 0x58495053  ("SPIX")
//     uint16_t version         = 1
//     uint16_t configHash      = FNV-1a hash of geometry + fontId
//     uint16_t pageCount       = number of entries that follow
//     uint16_t reserved        = 0 (padding)
//   Entries (pageCount × 8 bytes each):
//     uint32_t byteOffset
//     uint8_t  styleBits
//     uint8_t  reserved[3]     = 0
//
// Total size: 12 + N × 8 bytes.  100-page chapter = 812 bytes.  Sub-KB
// even for the longest chapters.  Negligible disk cost.
//
// CONFIG-HASH SEMANTICS:
//   Page boundaries depend on viewport geometry + font metrics.  If the
//   user changes font size, margin, or screen orientation, all .idx
//   files become stale (offsets would point to different positions in
//   the markers stream).  The configHash field lets the reader detect
//   stale .idx instantly and discard it.
//
//   Hash inputs (FNV-1a over a packed struct):
//     fontId, pageWidth, pageHeight, marginTop, marginBottom,
//     marginLeft, marginRight, bodyLineHeight, headingLineHeight,
//     paragraphSpacing
//
//   16-bit hash gives ~65k buckets — collisions possible but
//   astronomically unlikely to align with a legitimate config change.
// =============================================================================
constexpr uint32_t kPageIndexMagic = 0x58495053;
// v2.0.134: bumped 1 → 2.  v2.0.133 fixed a paginator bug where a word
// split across two `onText` calls (chunk boundary mid-word) was emitted
// as TWO words with a spurious inter-word space between them — line
// widths were inflated by ~spaceWidth per chunk boundary.  The
// post-fix paginator packs words tighter, so page boundaries land at
// DIFFERENT byte offsets than the pre-fix paginator produced.
//
// v2.0.137: bumped 2 → 3.  Fix changed how `byteOffset` is computed at
// page boundaries — now `bytesSeen_ - pendingBytes()` instead of just
// `bytesSeen_`, so resume points to where the carry+spillover bytes
// actually start in source, not past them.  Old v2 offsets are off by
// `pendingBytes()` per boundary — resuming from them lands beyond the
// dropped content, producing visible "pages don't connect" gaps.
//
// v2.0.140: bumped 3 → 4.  Fix corrected byteOffset when the carry word
// came from a partial buffered across a chunk boundary AND a non-text
// event (line break, anchor, image ref, style toggle) fired before
// flushPartWord triggered the carry.  Pre-fix, byteOffset overshot the
// carry word's first byte by the event-marker size (2 / 3 / 4 bytes
// depending on the event).  Russian / 2-byte-UTF-8 text resuming from
// such offsets landed mid-codepoint — the continuation byte rendered
// as `?` and the next char became the visible first glyph at every
// affected page boundary.  Visible as `?o написать` instead of
// `что написать` in the «Гибкий ум» EPUB at page boundaries near
// inline anchors.  Old v3 offsets are off by the event-marker size
// for those specific boundaries (others are correct); rebuild from
// scratch to get accurate values.
//
// v2.0.141: bumped 4 → 5.  Fix corrected a PRE-EXISTING bug where
// `PageCountingObserver` started `bytesSeen_` at 0 regardless of
// whether the underlying file was seeked.  When the caller resumed
// a render from a non-zero file offset (e.g. user navigated past
// the cold-walked range and the resume seeked to a cached page
// offset), all NEW boundary callbacks captured during that resume
// fired with byteOffsets RELATIVE to the seek start instead of
// absolute source positions.  Those wrong offsets got pushed into
// the `.idx` cache; subsequent page-turns seeked to grossly-wrong
// (way-too-early) source positions and the user saw mostly-identical
// content with only the top of the page changing as the seek
// offsets crept forward.  Symptom in v2.0.140 «Гибкий ум» trace:
// "при перелистывании менялись только первые три строки на экране"
// for pages past the cold walk's last cached entry.
//
// Fix: `MarkerizedRenderResume` gained a `byteOffset` field; caller
// passes the file seek position; `PageCountingObserver` initialises
// its `bytesSeen_` to that value, so all stored boundary offsets
// stay absolute.  Old v4 `.idx` files contain a mix of correct
// (cold-walk) and wrong (resume-captured) offsets — the wrong ones
// dominate after a few user navigations; rebuild from scratch.
// v2.0.142: bumped 5 → 6.  Fix corrected a second pre-existing bug
// where `currentSourceOffset_` wasn't restored before the spillover
// replay in `resetForNextPage`.  Any carry/partial captured DURING
// spillover processing recorded `carryStartByteOffset_` using the
// LAST onText's `currentSourceOffset_` (set when the carry-firing
// onText started), but spillover bytes are at a LATER source
// position (after the previous carry word).  Off-by-i bytes per
// affected boundary, where i = position-in-onText where saveSpillover
// started.  On-device symptom: words missing between certain page
// boundaries in long Russian chapters where text runs straddle the
// boundary AND spillover replay triggers a new carry.  Fix:
// `saveSpillover` now takes the absolute source offset of the saved
// bytes' first byte; `resetForNextPage` restores
// `currentSourceOffset_` to that value before replaying.
// v2.0.145: bumped 6 → 7.  Marker stream grew: HtmlStripper now emits
// kIndentOn/Off around `<li>` items and `<blockquote>` content,
// kCenterOn/Off around `<center>` (HTML) and `<title>` (FB2).  Page
// boundaries shift for any chapter whose source uses these tags
// (lists, blockquotes, FB2 chapter titles, centered HTML blocks).
// Old v6 indices are STALE for those chapters — drop and rebuild.
//
// v2.0.146: bumped 7 → 8.  Streaming paginator now honours the user's
// `fakeBold` setting (0=off, 1=bold, 2=extrabold) for measurement +
// drawing.  Pre-v146 indices were built with fakeBold implicitly off
// (paginator ignored the setting), so word widths recorded in those
// indices don't match what runtime render now produces when fakeBold
// is on.  Wrong widths → wrong page boundaries → resume seeks land
// off-by-some bytes (similar class as v2.0.140's symptoms).  Also
// folds fakeBold into `computePageIndexConfigHash` so future
// fakeBold setting changes auto-invalidate the cache.
// v2.0.147: bumped 8 → 9.  StreamingPaginator now treats a kPageBreak
// at chapter start (no content drawn yet) as a NO-OP instead of
// triggering immediate pageFull.  Pre-fix, FB2 chapters (which always
// start with `<section>` → kPageBreak) had an empty page 0; post-fix
// the title + first paragraph render as page 0.  Page boundaries
// shift by -1 for those chapters — old v8 indices are stale.  Same
// effect for HTML chapters whose source starts with stray
// `<hr>`-style page breaks.
// v2.0.206: bumped 9 → 10.  OFF-BY-ONE PAGE-COUNT FIX.  The MEASURE walk
// only fires a page-boundary callback on an OVERFLOW roll (page N fills →
// roll into page N+1), so a chapter spanning P pages produces only P-1
// boundaries (page 0 is implicit at offset 0; the final partial page ends
// at EOF and fires no roll).  R4.b emitted `entryCount` Pages — one short —
// so the LAST page of every multi-page chapter was never created and the
// reader jumped straight to the next spine, dropping the chapter's tail
// paragraph(s).  v2.0.175 had patched only the zero-boundary (single-page)
// case via a synthesised entry.  Fix: R4.b now emits `boundaryCount + 1`
// Pages and the synthesis is removed.  Old v9 indices carried the buggy
// count, so the bump forces a rebuild.  (The on-disk entry layout is
// unchanged — entries still record page 1..K starts, page 0 implicit — so
// resume seeking is untouched.)
constexpr uint16_t kPageIndexVersion = 10;
constexpr size_t   kPageIndexHeaderBytes = 12;
constexpr size_t   kPageIndexEntryBytes = 8;

// Compute a stable 16-bit hash over the paginator config + fontId
// + fakeBold mode.  Used as the `configHash` field in the .idx
// header to detect stale indices after font / margin / bold-mode
// changes (any of which shift page boundaries).
//
// v2.0.146 — fakeBold added as a hash input.  Pre-v146 indices
// hashed only cfg+fontId; their layout assumed fakeBold=0.  After
// the version bump (6 → 7 → 8) ALL old indices invalidate and
// rebuild with the correct fakeBold-aware widths.
uint16_t computePageIndexConfigHash(const StreamingPaginatorConfig& cfg, int fontId,
                                     uint8_t fakeBold = 0);

// Serialize a vector of PageBoundarySnapshot into a byte buffer.
// `outBuf` must have capacity for at least
// `kPageIndexHeaderBytes + entries.size() * kPageIndexEntryBytes`.
// Returns the number of bytes written, or 0 on buffer-too-small.
size_t serializePageIndex(const PageBoundarySnapshot* entries, size_t entryCount,
                          uint16_t configHash,
                          uint8_t* outBuf, size_t outBufSize);

// Deserialize page index from a byte buffer.  Returns true on success
// and fills `outEntries` (cleared first).  Returns false if:
//   * Buffer too small for header
//   * Magic mismatch (not a .idx file)
//   * Version mismatch (forward-incompatible)
//   * configHash mismatch (stale .idx — caller should delete + rebuild)
//   * Truncated entries (file corruption)
// Caller's `outEntries` is left empty on any failure.
bool deserializePageIndex(const uint8_t* buf, size_t bufSize,
                           uint16_t expectedConfigHash,
                           std::vector<PageBoundarySnapshot>& outEntries);

// Render `targetPage` (0-based) of the marker byte stream pulled from
// `readChunk` to the renderer's framebuffer.
//
// `paginator` carries the page geometry / fonts / margins config and the
// renderer (typically a `GfxRendererPaginatorAdapter`).  The function
// resets it for the chapter and toggles its draw-suppression bit
// internally — caller hands over a fresh paginator and gets back a
// paginator that has just rendered page `targetPage`.
//
// `chunkBuf` and `chunkBufSize` are the streaming chunk buffer.  Typical
// 1024-4096 bytes; larger means fewer LittleFS round-trips at the cost
// of stack space.
//
// `shouldAbort` (optional): cooperative cancel hook; checked between
// chunks.  Pass `{}` to disable.
//
// Returns:
//   * Success: target page was found AND drawn.  Caller should
//     immediately commit the framebuffer to the display.
//   * PageNotFound: stream EOF reached before page counter reached
//     `targetPage`.  Framebuffer state is undefined — typically partial
//     content of a page that doesn't exist.
//   * ReadError: source read returned negative.
//   * Aborted: shouldAbort fired.
//
// v2.0.125 R3.6 — `onPageBoundary` (optional) fires every time the
// paginator's currentPage_ rolls forward; caller can capture
// (pageIndex, byteOffset, styleBits) snapshots to enable direct
// seek on subsequent renders.  `resume` (optional) lets the caller
// SKIP the MEASURE-only walk through earlier pages by seeking the
// source to a known offset and telling the paginator its initial
// page index + style state.  See `PageBoundarySnapshot` and
// `MarkerizedRenderResume` doc above.
MarkerizedRenderResult renderMarkerizedPage(
    StreamingPaginator& paginator,
    ChapterReadFn readChunk,
    uint8_t* chunkBuf, size_t chunkBufSize,
    uint16_t targetPage,
    const ChapterAbortFn& shouldAbort = {},
    MarkerizedRenderStats* outStats = nullptr,
    const MarkerizedRenderResume& resume = {},
    const OnPageBoundaryFn& onPageBoundary = {});

}  // namespace snapix::smolport
