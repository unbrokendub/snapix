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

#include <RenderConfig.h>

#include <cstdint>
#include <string>

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

}  // namespace snapix::pagecache
