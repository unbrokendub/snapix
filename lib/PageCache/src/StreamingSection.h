#pragma once

// =============================================================================
// StreamingSection — shared "build the .idx + count pages" helper for
// single-section streamed documents (TXT, Markdown).
//
// EPUB/FB2 each have their own inline copy of the R4.c upfront-idx-build inside
// EpubChapterParser / Fb2Parser (per-spine / per-section).  TXT and Markdown are
// single-section (key 0, no images), so they share this one helper instead of
// duplicating the intricate MEASURE-walk + configHash + serialize logic.
//
// The Markers segment must already be written (the parser markerizes first).
// The paginator config built here MUST stay identical to the one
// ReaderState::renderPageContents builds for TXT/MD, or the idx configHash
// mismatches and every render rebuilds it.
// =============================================================================

#include <MarkerizedPageRender.h>  // PageBoundarySnapshot
#include <RenderConfig.h>

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

namespace snapix::pagecache {

// Ensure a CURRENT (version + configHash matching) .idx exists for the section's
// already-written Markers segment, building it via a MEASURE-only paginator walk
// if absent or stale.  Returns the total page count (>= 1) on success, or 0 if
// the Markers segment is missing or the markerizer is compiled out.
//
// `hyphenLang` selects the hyphenation dictionary ("" disables hyphenation).
uint16_t ensureStreamingSectionIdx(const std::string& bookCachePath, uint16_t sectionKey,
                                   GfxRenderer& renderer, const RenderConfig& config,
                                   int marginTop, int marginBottom, int marginLeft, int marginRight,
                                   const char* hyphenLang);

// =============================================================================
// v3.9.0 — incremental (lazy / progressive) idx for chunked single-section docs.
//
// A big TXT can't surface page 0 until the whole file is markerized + measured
// (~120 s for a 650 KB novel), because the markers live in ONE atomically-
// written segment.  Lazy markerize writes the markers in paragraph-bounded
// CHUNKS (Markers keys 0,1,2,…); after each chunk this function MEASURE-walks
// only the NEWLY-available pages — resuming from the last known page boundary
// over a ChunkedMarkersReader that spans Markers keys 0..N — and persists the
// growing idx.  Page 0 then renders after chunk 0 (~seconds), the rest fills in
// as the background cache loop markerizes more chunks.
//
// State carried across calls by the caller (PlainTextParser):
struct ChunkedIdxState {
  std::vector<snapix::smolport::PageBoundarySnapshot> boundaries;  // all page-start boundaries so far
  uint16_t configHash = 0;       // set on first call; render must match
  bool configHashValid = false;  // false until the first successful walk
};

// Measure the pages that became available after the latest markers chunk was
// written, appending their boundaries to `st`, and persist the full idx
// (Markers/Idx live in the book's streaming.cache under `bookCachePath`).
//
//   * Resumes the MEASURE walk from `st.boundaries.back()` (the last, still-
//     incomplete page) over Markers keys 0..N, so each call is ~O(one chunk).
//   * `sourceExhausted` true means the last chunk ran to true EOF, so the final
//     partial page now counts — returned page count gains the trailing +1.
//
// Returns the total page count available so far
// (= boundaries.size() + (sourceExhausted ? 1 : 0)), or 0 on failure / when the
// markerizer is compiled out.  `hyphenLang` selects hyphenation ("" disables).
uint16_t extendChunkedSectionIdx(ChunkedIdxState& st, const std::string& bookCachePath,
                                 bool sourceExhausted, GfxRenderer& renderer,
                                 const RenderConfig& config, int marginTop, int marginBottom,
                                 int marginLeft, int marginRight, const char* hyphenLang);

// Load the CURRENT (version + configHash matching) idx into `st` so a cold
// (post-reboot) parser skips re-measuring chunks it already indexed.  Returns
// the number of boundaries loaded, or 0 if no current idx exists (caller then
// measures from scratch via extendChunkedSectionIdx).
uint16_t loadChunkedSectionIdx(ChunkedIdxState& st, const std::string& bookCachePath,
                               GfxRenderer& renderer, const RenderConfig& config, int marginTop,
                               int marginBottom, int marginLeft, int marginRight,
                               const char* hyphenLang);

}  // namespace snapix::pagecache
