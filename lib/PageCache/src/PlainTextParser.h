#pragma once

#include <RenderConfig.h>
#include <SdFat.h>

#include <cstdint>
#include <string>

#include "ContentParser.h"
#include "StreamingSection.h"  // ChunkedIdxState

class GfxRenderer;
class Page;

/**
 * Content parser for plain-text files (.txt).
 *
 * v3.7.0 — migrated from the legacy ParsedText block path to the v3 streaming
 * pipeline (same as EPUB/FB2): markerize → MEASURE-walk idx → emit empty Pages
 * from the idx page count; ReaderState::renderPageContents reads the markers
 * directly for layout + drawing.
 *
 * v3.9.0 — LAZY / PROGRESSIVE markerize.  A 650 KB novel used to block ~120 s
 * (whole-file markerize + whole-file MEASURE) before page 0 appeared, because
 * the markers were one atomically-written segment.  Now the source is markerized
 * in paragraph-bounded CHUNKS (Markers keys 0,1,2,…), one chunk per parsePages
 * call, and the idx is extended incrementally over a ChunkedMarkersReader after
 * each chunk (see StreamingSection::extendChunkedSectionIdx).  Page 0 renders
 * after chunk 0 (~seconds); the background cache loop markerizes the rest.  A
 * small TXT (< one chunk) markerizes in a single chunk → identical to before.
 *
 * Reboot-safe with NO persisted resume cursor: a fresh parser probes how many
 * Markers chunks already exist and re-derives the source offset of the next
 * chunk by rescanning the source for paragraph boundaries (chunks tile the file
 * deterministically), then resumes markerizing / measuring from there.
 *
 * In a build without SNAPIX_MARKERIZER (build-test only) parsePages() produces
 * no pages, mirroring EpubChapterParser's stub behaviour.
 */
class PlainTextParser : public ContentParser {
  std::string filepath_;       // effective content path (SD or LittleFS UTF-8 cache)
  std::string bookCachePath_;  // UnifiedCache dir (Txt::getCachePath())
  GfxRenderer& renderer_;
  RenderConfig config_;
  bool useLittleFs_ = false;
  bool hasMore_ = true;

  // Progressive markerize state.
  bool initialized_ = false;       // ensureInit() ran (reflow + chunk cursor recovered)
  bool reflow_ = false;            // single '\n' → space (hard-wrapped text)
  uint32_t fileSize_ = 0;          // source byte length
  int chunkIdx_ = 0;               // next Markers chunk to write (key)
  uint32_t srcOffset_ = 0;         // source byte offset where chunk chunkIdx_ starts
  bool sourceExhausted_ = false;   // last chunk ran to EOF (no more to markerize)

  // Incremental idx + page-emit cursors.
  snapix::pagecache::ChunkedIdxState idxState_;  // boundaries measured so far
  uint16_t pagesAvailable_ = 0;    // pages the idx currently covers (grows)
  uint16_t emitCursor_ = 0;        // pages emitted to the page cache so far

  // Real viewport margins plumbed by the caller so the MEASURE-walk config
  // matches ReaderState::renderPageContents (else .idx configHash mismatches).
  int streamingViewportMarginTop_ = 4;
  int streamingViewportMarginBottom_ = 4;
  int streamingViewportMarginLeft_ = 4;
  int streamingViewportMarginRight_ = 4;

  // One-time init: detect reflow mode from a head sample and recover the chunk
  // cursor (chunkIdx_/srcOffset_/sourceExhausted_) + any existing idx so a cold
  // (post-reboot) parser resumes mid-document.  Returns false if the source
  // can't be opened.
  bool ensureInit();

  // Markerize the next source chunk [srcOffset_, end) into UnifiedCache::Markers
  // (key = chunkIdx_), where `end` is the next paragraph boundary past
  // CHUNK_BYTES (or EOF).  Advances srcOffset_/chunkIdx_ and sets
  // sourceExhausted_ when end == fileSize.  Returns true on success/cache-hit.
  bool markerizeNextChunk();

 public:
  // `bookCachePath` is the book's cache directory (Txt::getCachePath()), where
  // the per-book streaming.cache (markers + idx) lives.  `useLittleFs` selects
  // SD vs LittleFS for the source read (cp1251 UTF-8 cache lives on LittleFS).
  PlainTextParser(std::string filepath, std::string bookCachePath, GfxRenderer& renderer,
                  const RenderConfig& config, bool useLittleFs = false);
  ~PlainTextParser() override = default;

  bool parsePages(const std::function<void(std::unique_ptr<Page>)>& onPageComplete, uint16_t maxPages = 0,
                  const AbortCallback& shouldAbort = nullptr) override;
  bool hasMoreContent() const override { return hasMore_; }
  // Stay HOT (no cold rebuild) while initialized and there's anything left to
  // emit or markerize.  A fresh / reset parser returns false → the cache uses
  // the create / cold-extend path, which calls parsePages and re-inits.
  bool canResume() const override {
    return initialized_ && (emitCursor_ < pagesAvailable_ || !sourceExhausted_);
  }
  void reset() override;

  // Plumb real reader viewport margins (called right after construction), so the
  // upfront MEASURE walk uses the same paginator config as the render path.
  void setStreamingViewport(int marginTop, int marginBottom, int marginLeft, int marginRight) {
    streamingViewportMarginTop_ = marginTop;
    streamingViewportMarginBottom_ = marginBottom;
    streamingViewportMarginLeft_ = marginLeft;
    streamingViewportMarginRight_ = marginRight;
  }
};
