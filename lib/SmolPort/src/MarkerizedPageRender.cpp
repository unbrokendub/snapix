#include "MarkerizedPageRender.h"

#include <cstring>

#include "MarkerStream.h"

namespace snapix::smolport {

namespace {

// Wrapper observer that delegates every event to a `StreamingPaginator`,
// then checks `isPageFull()`.  When a page completes, increments an
// internal page counter and either:
//   * Resets the paginator to the next page (continuing layout); OR
//   * Returns Stop if we've completed the target page (we're done).
//
// The wrapper NEVER returns Stop in the middle of a page — that's the
// whole reason it exists.  Returning Stop mid-stream would leave bytes
// of the current chunk unconsumed (MarkerStreamReader.feed() doesn't
// report partial consumption), and since LittleFS reads are forward-
// only the caller would re-fetch the next chunk, losing the events
// that were after the Stop in the previous chunk.
//
// By absorbing the page-boundary signal here and only returning Stop
// when fully done, the byte stream flows continuously and the
// StreamingChapterRenderer just sees a clean ObserverStopped at the
// end of the target page.
//
// v2.0.125 R3.6 — also tracks bytes-seen-so-far per event and fires
// `onBoundary_` callback at every page rollover with a snapshot
// (pageIndex, byteOffset, styleBits).  Caller stores these to
// enable direct-seek resume on subsequent renders without the
// MEASURE-only walk through earlier pages.
class PageCountingObserver : public MarkerObserver {
 public:
  PageCountingObserver(StreamingPaginator& inner, uint16_t targetPage,
                        uint16_t startPage, uint32_t startByteOffset,
                        OnPageBoundaryFn onBoundary)
      : inner_(inner), targetPage_(targetPage), currentPage_(startPage),
        // v2.0.140 fix — when resuming from a non-zero file seek, start
        // `bytesSeen_` at the seek's source position so all boundary
        // offsets we capture remain ABSOLUTE.  Pre-fix this was 0 (the
        // default), so resume-captured offsets were relative to the
        // seek — cumulative resume drift on every page-turn past the
        // cold-walked range.
        bytesSeen_(startByteOffset),
        onBoundary_(std::move(onBoundary)) {
    // Page 0 is the initial page — start drawing immediately if target=0.
    inner_.setDrawSuppressed(currentPage_ != targetPage_);
  }

  uint16_t pagesAdvancedThrough() const { return currentPage_; }

  ObserverStatus onText(const uint8_t* t, size_t l) override {
    // v2.0.140 — push the source offset of the upcoming text run into
    // the paginator BEFORE the bytesSeen advance.  The paginator uses
    // this to compute absolute source offsets for words submitted from
    // the main loop (`currentSourceOffset_ + wordStart`) and for the
    // first byte of any partial word it buffers.  Without this, the
    // paginator can't tell where in source it is, and partial-word
    // carries via flushPartWord produce wrong `.idx` byteOffsets.
    inner_.setCurrentSourceOffset(bytesSeen_);
    bytesSeen_ += static_cast<uint32_t>(l);
    return checkAdvance(inner_.onText(t, l));
  }
  // Inline + block toggles + structural breaks are all 2 bytes
  // ([0x01, tag]).
  ObserverStatus onLineBreak() override        { bytesSeen_ += 2; return checkAdvance(inner_.onLineBreak()); }
  ObserverStatus onParagraphBreak() override   { bytesSeen_ += 2; return checkAdvance(inner_.onParagraphBreak()); }
  ObserverStatus onPageBreak() override        { bytesSeen_ += 2; return checkAdvance(inner_.onPageBreak()); }
  ObserverStatus onThematicBreak() override    { bytesSeen_ += 2; return checkAdvance(inner_.onThematicBreak()); }
  ObserverStatus onHeadingStart(uint8_t level) override {
    // [0x01, 'H', level_digit] — 3 bytes.
    bytesSeen_ += 3;
    return checkAdvance(inner_.onHeadingStart(level));
  }
  ObserverStatus onHeadingEnd() override   { bytesSeen_ += 2; return checkAdvance(inner_.onHeadingEnd()); }
  ObserverStatus onBoldStart() override    { bytesSeen_ += 2; return checkAdvance(inner_.onBoldStart()); }
  ObserverStatus onBoldEnd() override      { bytesSeen_ += 2; return checkAdvance(inner_.onBoldEnd()); }
  ObserverStatus onItalicStart() override  { bytesSeen_ += 2; return checkAdvance(inner_.onItalicStart()); }
  ObserverStatus onItalicEnd() override    { bytesSeen_ += 2; return checkAdvance(inner_.onItalicEnd()); }
  ObserverStatus onQuoteStart() override   { bytesSeen_ += 2; return checkAdvance(inner_.onQuoteStart()); }
  ObserverStatus onQuoteEnd() override     { bytesSeen_ += 2; return checkAdvance(inner_.onQuoteEnd()); }
  // v2.0.145 — Indent / Center markers.  Each is a 2-byte [01, tag].
  ObserverStatus onIndentStart() override  { bytesSeen_ += 2; return checkAdvance(inner_.onIndentStart()); }
  ObserverStatus onIndentEnd() override    { bytesSeen_ += 2; return checkAdvance(inner_.onIndentEnd()); }
  ObserverStatus onCenterStart() override  { bytesSeen_ += 2; return checkAdvance(inner_.onCenterStart()); }
  ObserverStatus onCenterEnd() override    { bytesSeen_ += 2; return checkAdvance(inner_.onCenterEnd()); }
  ObserverStatus onImageRef(const uint8_t* path, uint16_t len) override {
    // [0x01, 'M', len_lo, len_hi, ...path bytes].
    bytesSeen_ += static_cast<uint32_t>(4 + len);
    return checkAdvance(inner_.onImageRef(path, len));
  }
  ObserverStatus onAnchor(const uint8_t* id, uint16_t len) override {
    bytesSeen_ += static_cast<uint32_t>(4 + len);
    return checkAdvance(inner_.onAnchor(id, len));
  }
  ObserverStatus onStreamEnd() override {
    // Forward end-of-stream to paginator (flushes last partial line).
    inner_.onStreamEnd();
    return ObserverStatus::Continue;
  }

 private:
  StreamingPaginator& inner_;
  const uint16_t targetPage_;
  uint16_t currentPage_ = 0;
  // v2.0.140 fix — initialised in the ctor body from `resume.byteOffset`
  // so all captured boundaries remain ABSOLUTE source positions even
  // when the underlying file was seeked past the start of the chapter.
  uint32_t bytesSeen_ = 0;
  OnPageBoundaryFn onBoundary_;

  ObserverStatus checkAdvance(ObserverStatus innerStatus) {
    if (!inner_.isPageFull()) {
      // No page boundary yet — just propagate the inner status.  Note
      // we treat any inner Stop here as "the inner asked to abort but
      // not because of page-full" — pass it through.
      return innerStatus;
    }

    // v2.0.126 hotfix — fire the boundary callback FIRST, for the
    // page that's about to start (i.e. currentPage_ + 1), regardless
    // of whether we're stopping or continuing.  Pre-fix, when we
    // stopped at the target page, we never fired the callback for
    // target+1's boundary — so the cache always missed the most
    // recently rendered page.  Net effect: every subsequent render
    // resumed one page behind, walking through the most-just-
    // rendered page in MEASURE mode for no reason.
    //
    // The snapshot's `byteOffset` is bytesSeen_ AT THE END of the
    // last event of the current page MINUS any bytes the paginator
    // hasn't yet committed to a drawn slot.
    //
    // v2.0.137 fix — `bytesSeen_` blindly counts every byte handed
    // to `inner_.onText(...)`, but the paginator may return Stop
    // mid-text-run when `pageFull_` is hit.  Bytes for the carry
    // word AND the rest of the text run get saved in the paginator's
    // carry + spillover state — they belong to the NEXT page, not
    // the current one.  Without this subtraction, `.idx` byteOffsets
    // pointed PAST those bytes, so the resume path skipped a chunk
    // of content at every page boundary (visible as "pages don't
    // connect to each other" and occasional UTF-8 garbage when the
    // drop landed inside a multi-byte char).
    //
    // v2.0.140 fix — `bytesSeen_ - pendingBytes()` math was still wrong
    // when the carry came from a partial word buffered across a chunk
    // boundary AND a non-text event marker (line break, anchor, image
    // ref, style toggle) fired before flushPartWord triggered the
    // carry.  Those event marker bytes were already in `bytesSeen_`
    // but had no place in `pendingBytes()`, so byteOffset overshot by
    // exactly the event-marker size (2 / 3 / 4 bytes depending on the
    // event).  Russian text with 2-byte UTF-8 chars then landed mid-
    // codepoint on resume — the continuation byte rendered as `?` and
    // the next char appeared as the visible first glyph.  Authoring
    // pattern that produced this bug in real EPUBs:
    //   "...что<a id='anchor'/>написать..."
    //   where `<a>` is 4 bytes in the marker stream.
    // Fix: ask the paginator directly for the carry word's first-byte
    // source offset.  The paginator records this in `tryAddWord` when
    // it captures a carry; for partial-then-event carries the value
    // was set when the partial was first buffered (in an earlier
    // onText call) and preserved through subsequent event handlers.
    if (onBoundary_) {
      PageBoundarySnapshot snap;
      snap.pageIndex = static_cast<uint16_t>(currentPage_ + 1);
      if (inner_.carryWordLen() > 0) {
        // Carry exists — use its recorded source position.  This is
        // accurate regardless of where in the event sequence the
        // carry was captured.  Spillover bytes (if any) come from
        // the source AFTER the carry word, so seeking to this
        // offset and reading forward re-encounters them naturally.
        snap.byteOffset = inner_.carryStartByteOffset();
      } else {
        // No carry — pageFull fired from a cursor-only advance (e.g.
        // paragraph spacing pushed past the bottom margin).  Next
        // page's content starts at the current source position.
        // Spillover is 0 in this branch (saveSpillover is only ever
        // called after a carry).
        const uint32_t pending = inner_.pendingBytes();
        snap.byteOffset = (bytesSeen_ > pending) ? (bytesSeen_ - pending) : 0;
      }
      snap.styleBits = inner_.styleBits();
      // v3.5.2 — record whether the page about to start begins a paragraph.
      // At this point atParagraphStart_ is true iff the fill came from a
      // paragraph/thematic/page break (those set it before pageFull); a carry
      // or mid-line overflow leaves it false.  Restored on resume so the
      // first-line indent only appears on real paragraph starts.
      snap.atParagraphStart = inner_.atParagraphStart();
      onBoundary_(snap);
    }

    if (currentPage_ == targetPage_) {
      // We just finished the TARGET page.  Stop the stream — the
      // framebuffer now holds the rendered target-page pixels.
      // (Note: we do NOT increment currentPage_ here, so
      // `pagesAdvancedThrough()` still reports the target index for
      // log diagnostics — semantics preserved from v2.0.121.)
      return ObserverStatus::Stop;
    }

    // We're skipping pages — advance to the next one and continue.
    ++currentPage_;
    inner_.resetForNextPage();
    inner_.setDrawSuppressed(currentPage_ != targetPage_);
    return ObserverStatus::Continue;
  }
};

}  // namespace

MarkerizedRenderResult renderMarkerizedPage(StreamingPaginator& paginator, ChapterReadFn readChunk,
                                              uint8_t* chunkBuf, size_t chunkBufSize, uint16_t targetPage,
                                              const ChapterAbortFn& shouldAbort,
                                              MarkerizedRenderStats* outStats,
                                              const MarkerizedRenderResume& resume,
                                              const OnPageBoundaryFn& onPageBoundary) {
  if (!readChunk || chunkBuf == nullptr || chunkBufSize == 0) {
    if (outStats) {
      outStats->pagesAdvancedThrough = 0;
      outStats->bytesConsumed = 0;
      outStats->chunksProcessed = 0;
    }
    return MarkerizedRenderResult::ReadError;
  }

  // v2.0.125: caller may not pass a resume.startPage > targetPage —
  // that means caller already knows we're past the target.  Treat
  // as misconfiguration; render from scratch.
  const uint16_t effectiveStart = (resume.startPage <= targetPage) ? resume.startPage : 0;

  // Fresh-chapter reset.  Caller hands over a paginator constructed for
  // the page geometry; we ensure cursor is at the top of the content
  // area and any prior-render state is cleared.
  paginator.resetForChapter();
  // v2.0.125: replay style state from the resume snapshot so a
  // bold/italic span open across the resume point continues
  // correctly even though we skipped the events that opened it.
  if (effectiveStart > 0) {
    paginator.setStyleBits(resume.styleBits);
    // v3.5.2 — restore paragraph-start state for the resumed page so the
    // first-line indent isn't wrongly applied to a mid-sentence continuation
    // (resetForChapter above unconditionally set it true).
    paginator.setAtParagraphStart(resume.atParagraphStart);
  }

  // v2.0.140 fix — propagate the absolute source byte offset to
  // PageCountingObserver so boundary callbacks capture ABSOLUTE offsets
  // even when the caller seeked the file past the start of the chapter.
  // For cold walks (startPage == 0 default), this is 0 and behaviour is
  // identical to pre-fix.
  const uint32_t effectiveStartByteOffset =
      (effectiveStart > 0) ? resume.byteOffset : 0;
  PageCountingObserver counter(paginator, targetPage, effectiveStart,
                                effectiveStartByteOffset, onPageBoundary);
  StreamingChapterRenderer chapterRenderer(counter);

  RenderStats rstats{};
  const RenderStatus status =
      chapterRenderer.renderRange(readChunk, chunkBuf, chunkBufSize, shouldAbort, &rstats);
  // Always finish so the paginator's last partial line gets flushed.
  // If the observer asked Stop on the target page boundary, this is
  // a no-op for the inner paginator (its current page is empty).
  chapterRenderer.finish();

  if (outStats) {
    outStats->pagesAdvancedThrough = counter.pagesAdvancedThrough();
    outStats->bytesConsumed = rstats.bytesConsumed;
    outStats->chunksProcessed = rstats.chunksProcessed;
  }

  switch (status) {
    case RenderStatus::EofClean:
      // Stream ended cleanly.  Did we reach the target page?
      // PageCountingObserver only increments on full pages; the LAST
      // page of a chapter typically doesn't trigger isPageFull (it
      // ends mid-page on EOF).  So if `counter.pagesAdvancedThrough()`
      // == targetPage we DID reach the target (and onStreamEnd fired
      // the last-line flush); if it's less, the chapter was too short.
      if (counter.pagesAdvancedThrough() >= targetPage) {
        return MarkerizedRenderResult::Success;
      }
      return MarkerizedRenderResult::PageNotFound;
    case RenderStatus::ObserverStopped:
      // PageCountingObserver returned Stop after completing target.
      return MarkerizedRenderResult::Success;
    case RenderStatus::Aborted:
      return MarkerizedRenderResult::Aborted;
    case RenderStatus::ReadError:
    case RenderStatus::ReaderError:
      return MarkerizedRenderResult::ReadError;
  }
  return MarkerizedRenderResult::ReadError;
}

// =============================================================================
// v2.0.128 R4.a — page-offset index serde.
// =============================================================================

namespace {

// FNV-1a 32-bit, then folded to 16-bit by XORing high+low halves.
// Stable across architectures; no malloc.
inline uint32_t fnv1a32(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

inline void write_u16_le(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

inline void write_u32_le(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

inline uint16_t read_u16_le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_u32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

uint16_t computePageIndexConfigHash(const StreamingPaginatorConfig& cfg, int fontId,
                                     uint8_t fakeBold) {
  // Pack the inputs into a fixed-layout struct on the stack, then hash
  // the bytes.  Using a struct (rather than a stream of writes) ensures
  // consistent byte representation across builds.
  struct PackedConfig {
    int32_t  fontId;
    uint16_t pageWidth;
    uint16_t pageHeight;
    uint16_t marginTop;
    uint16_t marginBottom;
    uint16_t marginLeft;
    uint16_t marginRight;
    uint16_t bodyLineHeight;
    uint16_t headingLineHeight;
    uint16_t paragraphSpacing;
    uint16_t firstLineIndent;   // v3.1.0 — "красная строка" (layout-affecting)
    uint8_t  fakeBold;          // v2.0.146 — width-affecting setting
    uint8_t  hyphenate;         // v3.3.0 — word hyphenation on/off (layout-affecting)
    char     hyphenLang[8];     // v3.3.0 — dictionary selects break points
    uint8_t  reserved[2];        // pad to keep struct alignment stable
  } pc{};
  pc.fontId = fontId;
  pc.pageWidth = cfg.pageWidth;
  pc.pageHeight = cfg.pageHeight;
  pc.marginTop = cfg.marginTop;
  pc.marginBottom = cfg.marginBottom;
  pc.marginLeft = cfg.marginLeft;
  pc.marginRight = cfg.marginRight;
  pc.bodyLineHeight = cfg.bodyLineHeight;
  pc.headingLineHeight = cfg.headingLineHeight;
  pc.paragraphSpacing = cfg.paragraphSpacing;
  pc.firstLineIndent = cfg.firstLineIndent;
  pc.fakeBold = fakeBold;
  pc.hyphenate = cfg.hyphenate ? 1 : 0;
  std::memcpy(pc.hyphenLang, cfg.hyphenLang, sizeof(pc.hyphenLang));
  const uint32_t h32 = fnv1a32(reinterpret_cast<const uint8_t*>(&pc), sizeof(pc));
  // Fold 32→16 by XOR high half with low half.  Reduces hash collisions
  // for sequential changes (font-size +/-1 etc.) compared to taking
  // just the low 16 bits.
  return static_cast<uint16_t>((h32 & 0xffff) ^ (h32 >> 16));
}

size_t serializePageIndex(const PageBoundarySnapshot* entries, size_t entryCount,
                           uint16_t configHash,
                           uint8_t* outBuf, size_t outBufSize) {
  if (outBuf == nullptr) return 0;
  const size_t needed = kPageIndexHeaderBytes + entryCount * kPageIndexEntryBytes;
  if (outBufSize < needed) return 0;
  if (entryCount > UINT16_MAX) return 0;  // pageCount field is uint16

  // Header.
  write_u32_le(outBuf + 0, kPageIndexMagic);
  write_u16_le(outBuf + 4, kPageIndexVersion);
  write_u16_le(outBuf + 6, configHash);
  write_u16_le(outBuf + 8, static_cast<uint16_t>(entryCount));
  write_u16_le(outBuf + 10, 0);  // reserved

  // Entries.
  uint8_t* p = outBuf + kPageIndexHeaderBytes;
  for (size_t i = 0; i < entryCount; ++i) {
    write_u32_le(p + 0, entries[i].byteOffset);
    p[4] = entries[i].styleBits;
    p[5] = entries[i].atParagraphStart ? 1 : 0;  // v3.5.2
    p[6] = 0;
    p[7] = 0;
    // Note: pageIndex is implicit (entry N is page N+1 since R3.6
    // semantics are "snapshot captured when paginator rolled INTO
    // page i+1").  We don't store pageIndex — it's derived from
    // the entry's position in the array.
    p += kPageIndexEntryBytes;
  }
  return needed;
}

bool deserializePageIndex(const uint8_t* buf, size_t bufSize,
                           uint16_t expectedConfigHash,
                           std::vector<PageBoundarySnapshot>& outEntries) {
  outEntries.clear();
  if (buf == nullptr || bufSize < kPageIndexHeaderBytes) return false;

  const uint32_t magic = read_u32_le(buf + 0);
  const uint16_t version = read_u16_le(buf + 4);
  const uint16_t configHash = read_u16_le(buf + 6);
  const uint16_t pageCount = read_u16_le(buf + 8);

  if (magic != kPageIndexMagic) return false;
  if (version != kPageIndexVersion) return false;
  if (configHash != expectedConfigHash) return false;

  const size_t bodyBytes = static_cast<size_t>(pageCount) * kPageIndexEntryBytes;
  if (bufSize < kPageIndexHeaderBytes + bodyBytes) return false;  // truncated

  outEntries.reserve(pageCount);
  const uint8_t* p = buf + kPageIndexHeaderBytes;
  for (uint16_t i = 0; i < pageCount; ++i) {
    PageBoundarySnapshot snap{};
    // R3.6: entry N stores the boundary for "page N+1 starts here"
    // (after rendering page N completes).  pageIndex is 1-based.
    snap.pageIndex = static_cast<uint16_t>(i + 1);
    snap.byteOffset = read_u32_le(p + 0);
    snap.styleBits = p[4];
    snap.atParagraphStart = (p[5] != 0);  // v3.5.2
    outEntries.push_back(snap);
    p += kPageIndexEntryBytes;
  }
  return true;
}

}  // namespace snapix::smolport
