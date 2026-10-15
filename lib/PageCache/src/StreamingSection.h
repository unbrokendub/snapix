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

#include <ChunkedMarkersReader.h>  // MarkerChunkProvider
#include <MarkerizedPageRender.h>  // PageBoundarySnapshot

#include <RenderConfig.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class GfxRenderer;

namespace snapix::unifiedcache {
class UnifiedCache;
}

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
uint16_t ensureStreamingSectionIdx(snapix::unifiedcache::UnifiedCache& cache,
                                   uint16_t sectionKey, GfxRenderer& renderer,
                                   const RenderConfig& config, int marginTop, int marginBottom,
                                   int marginLeft, int marginRight, const char* hyphenLang);

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
//
// `resolveImage` (optional) lets the MEASURE walk account for inline-image
// heights so page boundaries match the render path — FB2 passes its
// cacheImage-backed resolver; TXT/MD (no inline images) leave it empty.
// `probeImage` (optional, v3.11.12) supplies the source-header layout size so
// it does not depend on whether the BMP is decoded yet.
uint16_t extendChunkedSectionIdx(ChunkedIdxState& st, const std::string& bookCachePath,
                                 bool sourceExhausted, GfxRenderer& renderer,
                                 const RenderConfig& config, int marginTop, int marginBottom,
                                 int marginLeft, int marginRight, const char* hyphenLang,
                                 std::function<std::string(const uint8_t*, size_t)> resolveImage = {},
                                 std::function<int(const uint8_t*, size_t, uint16_t&, uint16_t&)> probeImage = {});
uint16_t extendChunkedSectionIdx(
    ChunkedIdxState& st, snapix::unifiedcache::UnifiedCache& cache, bool sourceExhausted,
    GfxRenderer& renderer, const RenderConfig& config, int marginTop, int marginBottom,
    int marginLeft, int marginRight, const char* hyphenLang,
    std::function<std::string(const uint8_t*, size_t)> resolveImage = {},
    std::function<int(const uint8_t*, size_t, uint16_t&, uint16_t&)> probeImage = {});

// v3.11.12 — the incremental-idx core behind extendChunkedSectionIdx, for any
// section whose markers arrive in chunks (EPUB chapters use their own idx key,
// paginator config, image hooks and anchor capture).
struct ChunkedIdxWalk {
  uint16_t idxKey = 0;  // Idx segment key the grown idx is written under
  snapix::smolport::StreamingPaginatorConfig cfg{};
  uint16_t configHash = 0;
  // The provider's chunks end at the document's end: the trailing partial page
  // counts and the walk flushes the last word.  False while more chunks will
  // follow — see renderMarkerizedPage(streamComplete).
  bool streamComplete = false;
  std::function<std::string(const uint8_t*, size_t)> resolveImage;
  std::function<int(const uint8_t*, size_t, uint16_t&, uint16_t&)> probeImage;
  // Anchors met by this walk; it starts at the last boundary's page, so the
  // caller drops anchors it recorded for pages >= that page beforehand.
  snapix::smolport::OnAnchorPageFn onAnchorPage;
  std::function<bool()> shouldAbort;
};

struct ChunkedIdxExtend {
  uint16_t totalPages = 0;  // boundaries + (reachedEnd ? 1 : 0)
  uint16_t added = 0;       // boundaries appended by this walk
  bool reachedEnd = false;  // streamComplete and the walk got to the end
  bool aborted = false;     // shouldAbort fired (boundaries so far are kept)
  bool ok = false;          // false: no markers / read error / alloc failure
};

// Resume from st.boundaries.back() over the provider's chunks, append the new
// boundaries and persist the whole idx under walk.idxKey.  Boundaries found
// before an abort are final and kept.
ChunkedIdxExtend extendChunkedIdx(ChunkedIdxState& st, snapix::unifiedcache::UnifiedCache& cache,
                                  snapix::smolport::MarkerChunkProvider& provider, GfxRenderer& renderer,
                                  const RenderConfig& config, const ChunkedIdxWalk& walk);

// Load the CURRENT (version + configHash matching) idx into `st` so a cold
// (post-reboot) parser skips re-measuring chunks it already indexed.  Returns
// the number of boundaries loaded, or 0 if no current idx exists (caller then
// measures from scratch via extendChunkedSectionIdx).
uint16_t loadChunkedSectionIdx(ChunkedIdxState& st, const std::string& bookCachePath,
                               GfxRenderer& renderer, const RenderConfig& config, int marginTop,
                               int marginBottom, int marginLeft, int marginRight,
                               const char* hyphenLang);
uint16_t loadChunkedSectionIdx(ChunkedIdxState& st,
                               snapix::unifiedcache::UnifiedCache& cache,
                               GfxRenderer& renderer, const RenderConfig& config, int marginTop,
                               int marginBottom, int marginLeft, int marginRight,
                               const char* hyphenLang);
// v3.11.12 — same for an explicit idx key + config hash (EPUB chapters).
uint16_t loadChunkedIdx(ChunkedIdxState& st, snapix::unifiedcache::UnifiedCache& cache, uint16_t idxKey,
                        uint16_t configHash);

}  // namespace snapix::pagecache
