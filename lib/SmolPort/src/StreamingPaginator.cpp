#include "StreamingPaginator.h"

#include <algorithm>
#include <cstring>

namespace snapix::smolport {

// =============================================================================
// Construction + lifecycle
// =============================================================================
StreamingPaginator::StreamingPaginator(const StreamingPaginatorConfig& cfg, PaginatorRenderer& renderer)
    : cfg_(cfg), renderer_(renderer) {
  // v2.0.151 — heap-allocate the 2 KB spillover buffer once at
  // construction (rather than per resetForChapter) so it lives
  // for the paginator's whole lifetime — saves a malloc/free on
  // every chapter boundary.
  spilloverBuf_ = std::unique_ptr<uint8_t[]>(new uint8_t[kSpilloverCapacity]);
  spilloverCapacity_ = kSpilloverCapacity;
  resetForChapter();
}

void StreamingPaginator::resetForChapter() {
  cursorX_ = cfg_.marginLeft;
  cursorY_ = cfg_.marginTop;
  pageFull_ = false;
  lineHasContent_ = false;
  styleBits_ = 0;
  lineTextLen_ = 0;
  lineWordCount_ = 0;
  carryWordLen_ = 0;
  carryWordWidth_ = 0;
  carryWordStyle_ = 0;
  // v2.0.133 — drop any partial-word buffer from a previous chapter.
  partWordLen_ = 0;
  // v2.0.135 — clear inline-style-toggle continuation flag.
  partWordContinuation_ = false;
  // v2.0.137 — drop any spillover bytes from a previous chapter.
  spilloverLen_ = 0;
  // v2.0.140 — drop any in-flight byte-offset tracking from previous chapter.
  currentSourceOffset_ = 0;
  partialStartByteOffset_ = 0;
  carryStartByteOffset_ = 0;
  // v2.0.141 — also clear spillover source offset.
  spilloverSourceOffset_ = 0;
  // v2.0.145 — drop indent/center state; no pending image.
  indentDepth_ = 0;
  centered_ = false;
  // v3.1.0 — the chapter opens a fresh paragraph → its first line indents.
  atParagraphStart_ = true;
  lineIndent_ = 0;
  pendingImagePathLen_ = 0;
  pendingImageWidth_ = 0;
  pendingImageHeight_ = 0;
}

void StreamingPaginator::resetForNextPage() {
  cursorX_ = cfg_.marginLeft;
  cursorY_ = cfg_.marginTop;
  pageFull_ = false;
  // Style state is preserved across page boundaries (a word in the middle
  // of a bold run that flows onto a new page stays bold).  Carry-over word
  // is also preserved — it's the first word of the new page.
  // v2.0.135 — clear continuation flag at hard page break.  A word
  // carried to the new page starts at the left margin as the line's
  // first slot; no inter-word space context to preserve.
  partWordContinuation_ = false;
  // v2.0.145 — if a previous page's image-ref couldn't fit, draw it now
  // before any text on the new page.  Image dimensions were resolved at
  // capture time so we just paint and advance.
  if (pendingImagePathLen_ > 0 && pendingImageHeight_ > 0) {
    const uint16_t imgW = pendingImageWidth_;
    const uint16_t imgH = pendingImageHeight_;
    const uint16_t verticalAvail = verticalRemaining();
    if (imgH <= verticalAvail) {
      if (!drawSuppressed_) {
        const uint16_t effectiveLeft = static_cast<uint16_t>(
            cfg_.marginLeft + indentDepth_ * kIndentStep);
        const uint16_t innerW = static_cast<uint16_t>(
            cfg_.pageWidth - cfg_.marginRight - effectiveLeft);
        const uint16_t imgX = (innerW > imgW)
                                  ? static_cast<uint16_t>(effectiveLeft + (innerW - imgW) / 2)
                                  : effectiveLeft;
        renderer_.drawImage(imgX, cursorY_, imgW, imgH,
                              pendingImagePath_, pendingImagePathLen_);
      }
      cursorY_ = static_cast<uint16_t>(cursorY_ + imgH);
      if (cursorY_ + currentLineHeight() > cfg_.pageHeight - cfg_.marginBottom) {
        pageFull_ = true;
      }
    }
    // Clear pending state regardless — if it still doesn't fit on a
    // fresh page (oversized image), we drop it rather than loop forever.
    pendingImagePathLen_ = 0;
    pendingImageWidth_ = 0;
    pendingImageHeight_ = 0;
  }
  emitCarryWord();
  // v2.0.137 — process spillover bytes from previous page.  These are
  // bytes that came AFTER the carry word in the same onText call but
  // couldn't be delivered because we'd already returned Stop.  Run
  // them through the normal text path now — they'll tokenize into
  // words and add slots after the carry word.  May trigger pageFull
  // again, in which case spillover is re-saved for the page after.
  if (spilloverLen_ > 0) {
    const uint16_t len = spilloverLen_;
    // v2.0.207 — move the spillover buffer OUT (heap, not a stack copy) so it
    // can exceed the initial kSpilloverCapacity without inflating the stack
    // frame (the old `uint8_t buf[kSpilloverCapacity]` was 2 KB on a stack
    // already tight enough to need a 24 KB loopTask).  Re-saves triggered
    // during the replay below lazily re-allocate spilloverBuf_ via
    // saveSpillover; `replay` frees on scope exit.
    std::unique_ptr<uint8_t[]> replay = std::move(spilloverBuf_);
    const size_t replayCapacity = spilloverCapacity_;  // v3.1.1 — for reuse below
    spilloverCapacity_ = 0;
    spilloverLen_ = 0;  // clear FIRST so re-entrant save doesn't conflict
    // v2.0.141 — restore currentSourceOffset_ to where the spillover
    // bytes ACTUALLY start in source.  Without this, any tryAddWord
    // inside the spillover replay records carryStartByteOffset_ using
    // the LAST onText's currentSourceOffset_ (set when the carry-firing
    // onText was processed), not where the spillover bytes are in
    // source — those addresses differ by the position-in-onText where
    // saveSpillover was called from.  Symptom on-device: pages
    // captured during spillover replay store wrong (too-early)
    // byteOffsets in `.idx`; resume seeks before the actual page
    // boundary and reads duplicate content from the previous page.
    currentSourceOffset_ = spilloverSourceOffset_;
    spilloverSourceOffset_ = 0;
    (void)processTextRun(replay.get(), len);
    // v3.1.1 — reuse instead of free.  If the replay didn't trigger a new
    // save (the common case: everything fit on the fresh page),
    // spilloverBuf_ is still null — hand the old buffer back so the next
    // page boundary doesn't pay a fresh malloc.  Kills the per-boundary
    // alloc/free churn the v3.0.2 move-out introduced, without giving up
    // its stack-frame win.
    if (!spilloverBuf_) {
      spilloverBuf_ = std::move(replay);
      spilloverCapacity_ = replayCapacity;
    }
  }
}

// =============================================================================
// MarkerObserver event handlers
// =============================================================================

ObserverStatus StreamingPaginator::onText(const uint8_t* text, size_t len) {
  if (text == nullptr || len == 0) {
    return pageFull_ ? ObserverStatus::Stop : ObserverStatus::Continue;
  }
  if (pageFull_) {
    // v2.0.137 — caller's onText bytes can't be processed on this page
    // (it's full).  Save them as spillover so the next page sees them
    // first.
    // v2.0.141 — source position of text[0] is currentSourceOffset_
    // (this is the start of the new onText, no offset).  Go through
    // saveSpillover so spilloverSourceOffset_ gets recorded.
    saveSpillover(text, len, currentSourceOffset_);
    return ObserverStatus::Stop;
  }
  return processTextRun(text, len);
}

ObserverStatus StreamingPaginator::processTextRun(const uint8_t* text, size_t len) {
  // v2.0.135 — if this text starts with whitespace, the source had a
  // normal word break between the previously-flushed partial (if any)
  // and the upcoming word.  Drop any inline-style continuation flag
  // raised earlier; the next word gets normal leading-space treatment.
  if (isAsciiSpace(text[0])) {
    partWordContinuation_ = false;
  }

  // v2.0.133 — partial-word continuation: if the previous `onText` call
  // left an incomplete word in `partWordBuf_` (chunk boundary fell mid-
  // word), the FIRST non-whitespace bytes of this call belong to that
  // word.  Greedily gobble onto the buffer until either whitespace
  // arrives (partial is complete) OR the buffer fills (flush truncated
  // and start fresh) OR the input runs out (still partial, return).
  //
  // CRITICAL for Russian/CJK text: without this, a chunk boundary
  // inside the 2-byte UTF-8 sequence of a Cyrillic char would emit the
  // lead byte (0xD0/0xD1) as the END of one "word" and the
  // continuation byte (0x80-0xBF) as the START of the next, both
  // rendering as latin-1 fallback glyphs (ÿ°/ÿÿ etc.).
  size_t i = 0;
  if (partWordLen_ > 0) {
    while (i < len && !isAsciiSpace(text[i]) && partWordLen_ < kPartWordCapacity) {
      partWordBuf_[partWordLen_++] = text[i++];
    }
    if (i < len && isAsciiSpace(text[i])) {
      // Partial assembled into a complete word.  Submit it; whitespace
      // at i will be consumed by the main loop below.
      // v2.0.135: direct tryAddWord (NOT flushPartWord) — whitespace
      // naturally separates from the next word, no continuation glue.
      // v2.0.140: pass partialStartByteOffset_ so a carry from this
      // partial gets the correct source position for `.idx`.
      if (!tryAddWord(partWordBuf_, partWordLen_, partialStartByteOffset_)) {
        partWordLen_ = 0;
        partialStartByteOffset_ = 0;
        // v2.0.137 — carry took the partial word; spillover gets bytes
        // from `i` onwards (after the whitespace at i).
        // v2.0.141 — source position of text[i] = currentSourceOffset_ + i.
        saveSpillover(text + i, len - i, currentSourceOffset_ + i);
        return ObserverStatus::Stop;
      }
      partWordLen_ = 0;
      partialStartByteOffset_ = 0;
      ++i;  // consume the whitespace we found
    } else if (i >= len) {
      // Still partial — input exhausted before any whitespace.  Wait
      // for the next onText call.  partialStartByteOffset_ is preserved
      // (first byte still where it was when buffered).
      return ObserverStatus::Continue;
    } else {
      // Buffer overflowed but text continues.  Flush what we have as a
      // (truncated) word and let the main loop pick up the overflow
      // bytes as a new word.  Pathological case for words > 128 bytes;
      // real text never hits this.
      // v2.0.135: source has NO whitespace between the truncated half
      // and the upcoming bytes — set continuation so the next slot
      // glues without spaceWidth.
      if (!tryAddWord(partWordBuf_, partWordLen_, partialStartByteOffset_)) {
        partWordLen_ = 0;
        partialStartByteOffset_ = 0;
        // v2.0.141 — source position of text[i] = currentSourceOffset_ + i.
        saveSpillover(text + i, len - i, currentSourceOffset_ + i);
        return ObserverStatus::Stop;
      }
      partWordLen_ = 0;
      partialStartByteOffset_ = 0;
      partWordContinuation_ = true;
    }
  }

  // Walk the rest of the text run, emitting one word at a time.  A "word"
  // is a maximal run of non-whitespace bytes; whitespace separates words.
  // The run pointer is only valid for this call so we copy each word
  // immediately (either into the line buffer via tryAddWord, or into
  // partWordBuf_ if it's the trailing incomplete word).
  while (i < len) {
    // Skip leading whitespace.
    while (i < len && isAsciiSpace(text[i])) {
      // Inter-word space: signals "next word will need a leading space"
      // when added to the line.  tryAddWord() handles that gap.
      ++i;
    }
    if (i >= len) break;

    // Find the end of this word.
    const size_t wordStart = i;
    while (i < len && !isAsciiSpace(text[i])) {
      ++i;
    }
    const size_t wordLen = i - wordStart;

    if (i >= len) {
      // Word extends to the end of this text run — it may be incomplete
      // (continued in the next onText call).  Buffer instead of submit.
      if (wordLen <= kPartWordCapacity) {
        std::memcpy(partWordBuf_, text + wordStart, wordLen);
        partWordLen_ = static_cast<uint16_t>(wordLen);
        // v2.0.140 — record absolute source position of this partial's
        // first byte; will be used as carryStartByteOffset_ if the
        // partial later gets carried via flushPartWord from a
        // non-text event handler.
        partialStartByteOffset_ = currentSourceOffset_ + wordStart;
        return ObserverStatus::Continue;
      }
      // Oversized: submit as-is.  Buffer would truncate so prefer the
      // renderer's clipping over data loss.  Very rare in real text.
      if (!tryAddWord(text + wordStart, wordLen, currentSourceOffset_ + wordStart)) {
        // v2.0.137 — carry took this word; no bytes left after it
        // (i >= len), so spillover stays empty.
        return ObserverStatus::Stop;
      }
      return ObserverStatus::Continue;
    }

    // Whitespace was found after the word — it's definitely complete.
    if (!tryAddWord(text + wordStart, wordLen, currentSourceOffset_ + wordStart)) {
      // Page full — caller should stop feeding events.  The word that
      // didn't fit is now in the carry slot for the next page.
      // v2.0.137 — save bytes AFTER this carried word (starting at i,
      // which is the whitespace right after the failed word) into
      // spillover.  On next page, after emitCarryWord, resetForNextPage
      // will process spillover so the source-order tail of this text
      // run shows up immediately after the carry word.
      // v2.0.141 — source position of text[i] = currentSourceOffset_ + i.
      saveSpillover(text + i, len - i, currentSourceOffset_ + i);
      return ObserverStatus::Stop;
    }
  }

  return ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onLineBreak() {
  if (pageFull_) return ObserverStatus::Stop;
  // v2.0.133 — flush any partial word BEFORE structural break, else the
  // word is lost (or worse, gets glued onto the next paragraph's first
  // word).
  flushPartWord();
  flushLine();
  return pageFull_ ? ObserverStatus::Stop : ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onParagraphBreak() {
  if (pageFull_) return ObserverStatus::Stop;
  flushPartWord();  // v2.0.133
  flushLine();
  // v3.1.0 — the next line begins a new paragraph → indent its first line.
  atParagraphStart_ = true;
  // Add inter-paragraph spacing.  Cursor advanced by one line height in
  // flushLine; add the configured paragraph extra.
  cursorY_ = static_cast<uint16_t>(cursorY_ + cfg_.paragraphSpacing);
  if (cursorY_ + currentLineHeight() > cfg_.pageHeight - cfg_.marginBottom) {
    pageFull_ = true;
  }
  return pageFull_ ? ObserverStatus::Stop : ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onPageBreak() {
  // v2.0.147 — detect fresh-page state BEFORE flushing.  If nothing's
  // been laid out yet (FB2 chapter just opened, first event is
  // `<section>` → kPageBreak), treat the break as a no-op and let
  // the chapter's actual content land on this page.  Without this,
  // every FB2 chapter rendered an empty page 0 because the
  // page-break-at-start immediately tripped pageFull, the boundary
  // callback advanced currentPage to 1, and the title+body went to
  // page 1.  Same fix applies to HTML chapters starting with stray
  // `<hr>`-style page-break markers.
  const bool freshPage =
      cursorY_ == cfg_.marginTop && !lineHasContent_ && lineWordCount_ == 0 &&
      partWordLen_ == 0 && carryWordLen_ == 0 && spilloverLen_ == 0 &&
      pendingImagePathLen_ == 0;
  if (freshPage) {
    return ObserverStatus::Continue;
  }
  // Hard page break — flush whatever's accumulated and mark page full.
  flushPartWord();  // v2.0.133
  flushLine();
  // v3.1.0 — content after a hard break (new section/chapter) opens a fresh
  // paragraph; its first line on the next page indents.
  atParagraphStart_ = true;
  pageFull_ = true;
  return ObserverStatus::Stop;
}

ObserverStatus StreamingPaginator::onThematicBreak() {
  if (pageFull_) return ObserverStatus::Stop;
  flushPartWord();  // v2.0.133
  flushLine();

  // v3.2.0 — draw a centered scene-break ornament ("* * *") on its own line
  // instead of leaving an invisible gap (EPUB <hr> previously rendered as
  // nothing).  ASCII asterisks are guaranteed present in every bundled font
  // (a single U+2042 ASTERISM glyph is not).  This runs in BOTH the MEASURE
  // walk and the DRAW pass: cursorY_ advances identically in both (the fit
  // guard reads only cursorY_, which is draw-mode-independent), and only the
  // drawWord is gated on drawSuppressed_ — so .idx page boundaries match
  // what the render pass produces.
  const uint16_t bottom = static_cast<uint16_t>(cfg_.pageHeight - cfg_.marginBottom);
  cursorY_ = static_cast<uint16_t>(cursorY_ + cfg_.paragraphSpacing);  // air above
  if (cursorY_ + cfg_.bodyLineHeight <= bottom) {
    if (!drawSuppressed_) {
      static const uint8_t kOrnament[] = {'*', ' ', '*', ' ', '*'};
      const uint16_t w = renderer_.measureWidth(kOrnament, sizeof(kOrnament), 0);
      const uint16_t innerW =
          static_cast<uint16_t>(cfg_.pageWidth - cfg_.marginLeft - cfg_.marginRight);
      const uint16_t x = (innerW > w)
                             ? static_cast<uint16_t>(cfg_.marginLeft + (innerW - w) / 2)
                             : cfg_.marginLeft;
      renderer_.drawWord(x, cursorY_, kOrnament, sizeof(kOrnament), 0);
    }
    cursorY_ = static_cast<uint16_t>(cursorY_ + cfg_.bodyLineHeight);  // consume ornament line
  }
  // (If the ornament line doesn't fit at the page bottom it's skipped — the
  // break still functions via the spacing + new-paragraph indent below; the
  // divider just isn't redrawn on the next page.  Rare and benign.)

  // v3.1.0 — a thematic break (scene divider) starts a new paragraph.
  atParagraphStart_ = true;
  cursorY_ = static_cast<uint16_t>(cursorY_ + cfg_.paragraphSpacing);  // air below
  if (cursorY_ + currentLineHeight() > bottom) {
    pageFull_ = true;
  }
  return pageFull_ ? ObserverStatus::Stop : ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onHeadingStart(uint8_t /*level*/) {
  // v2.0.133 — flush partial under OLD style before transition.
  flushPartWord();
  styleBits_ = static_cast<uint8_t>(styleBits_ | kStyleHeading);
  return ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onHeadingEnd() {
  flushPartWord();  // v2.0.133
  styleBits_ = static_cast<uint8_t>(styleBits_ & ~kStyleHeading);
  return ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onBoldStart() {
  flushPartWord();  // v2.0.133
  styleBits_ = static_cast<uint8_t>(styleBits_ | kStyleBold);
  return ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onBoldEnd() {
  flushPartWord();  // v2.0.133
  styleBits_ = static_cast<uint8_t>(styleBits_ & ~kStyleBold);
  return ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onItalicStart() {
  flushPartWord();  // v2.0.133
  styleBits_ = static_cast<uint8_t>(styleBits_ | kStyleItalic);
  return ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onItalicEnd() {
  flushPartWord();  // v2.0.133
  styleBits_ = static_cast<uint8_t>(styleBits_ & ~kStyleItalic);
  return ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onQuoteStart() {
  flushPartWord();  // v2.0.133
  styleBits_ = static_cast<uint8_t>(styleBits_ | kStyleQuote);
  return ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onQuoteEnd() {
  flushPartWord();  // v2.0.133
  styleBits_ = static_cast<uint8_t>(styleBits_ & ~kStyleQuote);
  return ObserverStatus::Continue;
}

// v2.0.145 — Indent on/off.  Pushes the effective left margin in by
// `kIndentStep` per level so list items / nested quotes visually
// offset from the surrounding paragraph margin.  Stacks up to
// `kMaxIndentDepth` levels; deeper opens clamp at max.
//
// Indent transition mid-line is awkward (the line is already laid out
// at the OLD margin), so we flush the current line first if it has
// content.  This means an indent toggle inside a word splits visually
// at that word boundary — same convention as quote/heading toggles.
ObserverStatus StreamingPaginator::onIndentStart() {
  if (pageFull_) return ObserverStatus::Stop;
  flushPartWord();
  // Flush any in-progress line so the new indent applies to fresh
  // line layout.  Don't justify — this is a mid-paragraph break.
  if (lineWordCount_ > 0 || lineHasContent_) {
    flushLine(/*justify=*/false);
  }
  if (indentDepth_ < kMaxIndentDepth) {
    ++indentDepth_;
  }
  // Cursor restarts at new effective left margin for next word.
  cursorX_ = static_cast<uint16_t>(cfg_.marginLeft + indentDepth_ * kIndentStep);
  return pageFull_ ? ObserverStatus::Stop : ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onIndentEnd() {
  if (pageFull_) return ObserverStatus::Stop;
  flushPartWord();
  if (lineWordCount_ > 0 || lineHasContent_) {
    flushLine(/*justify=*/false);
  }
  if (indentDepth_ > 0) {
    --indentDepth_;
  }
  cursorX_ = static_cast<uint16_t>(cfg_.marginLeft + indentDepth_ * kIndentStep);
  return pageFull_ ? ObserverStatus::Stop : ObserverStatus::Continue;
}

// v2.0.145 — Center on/off.  Sets a flag consumed by flushLine to
// center-align (instead of left+justify) any line flushed while the
// flag is true.  Flush the current line first so the alignment
// change is clean.
ObserverStatus StreamingPaginator::onCenterStart() {
  if (pageFull_) return ObserverStatus::Stop;
  flushPartWord();
  if (lineWordCount_ > 0 || lineHasContent_) {
    flushLine(/*justify=*/false);
  }
  centered_ = true;
  return pageFull_ ? ObserverStatus::Stop : ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onCenterEnd() {
  if (pageFull_) return ObserverStatus::Stop;
  flushPartWord();
  // Last centered line: flush it WITHOUT justification (already the
  // default for centered_=true), then turn off the flag.
  if (lineWordCount_ > 0 || lineHasContent_) {
    flushLine(/*justify=*/false);
  }
  centered_ = false;
  return pageFull_ ? ObserverStatus::Stop : ObserverStatus::Continue;
}

// v2.0.145 — Image reference.  Resolve dimensions via the renderer;
// if the image fits in the remaining vertical space, flush the
// current text line and draw the image on its own line(s), advancing
// cursorY past it.  If it doesn't fit, set pageFull and stash the
// image path so the next page emits it via flushLine before any
// text.  Images are NEVER carried as text words — they get their
// own dedicated band.
//
// Path is copied because the marker stream's payload buffer is
// reused across calls.
ObserverStatus StreamingPaginator::onImageRef(const uint8_t* path, uint16_t len) {
  if (pageFull_) return ObserverStatus::Stop;
  if (path == nullptr || len == 0) return ObserverStatus::Continue;

  // Flush any in-progress text so the image draws on a fresh line.
  flushPartWord();
  if (lineWordCount_ > 0 || lineHasContent_) {
    flushLine(/*justify=*/false);
    if (pageFull_) {
      // Line break filled the page → stash image for next page.
      const uint16_t toCopy = (len < kImagePathCapacity - 1)
                                  ? len
                                  : static_cast<uint16_t>(kImagePathCapacity - 1);
      std::memcpy(pendingImagePath_, path, toCopy);
      pendingImagePathLen_ = toCopy;
      pendingImagePath_[toCopy] = '\0';
      return ObserverStatus::Stop;
    }
  }

  // Resolve image dimensions via the renderer.  Default no-op renderer
  // returns false → image is silently skipped (preserves pre-v145
  // behaviour for renderers that don't know about images).
  uint16_t imgW = 0, imgH = 0;
  const bool resolved = renderer_.resolveImageSize(path, len, &imgW, &imgH);
  if (!resolved || imgW == 0 || imgH == 0) {
    return ObserverStatus::Continue;
  }

  // Clamp width to the working area (renderer should already scale,
  // but defense in depth — clip if oversized).
  const uint16_t maxW = static_cast<uint16_t>(cfg_.pageWidth -
                                                cfg_.marginLeft -
                                                cfg_.marginRight);
  if (imgW > maxW) imgW = maxW;

  const uint16_t verticalAvail = verticalRemaining();
  if (imgH > verticalAvail) {
    // Image doesn't fit on this page → stash for next page and stop.
    const uint16_t toCopy = (len < kImagePathCapacity - 1)
                                ? len
                                : static_cast<uint16_t>(kImagePathCapacity - 1);
    std::memcpy(pendingImagePath_, path, toCopy);
    pendingImagePathLen_ = toCopy;
    pendingImagePath_[toCopy] = '\0';
    pendingImageWidth_ = imgW;
    pendingImageHeight_ = imgH;
    pageFull_ = true;
    return ObserverStatus::Stop;
  }

  // Draw image centered horizontally within the indented working width.
  if (!drawSuppressed_) {
    const uint16_t effectiveLeft = static_cast<uint16_t>(
        cfg_.marginLeft + indentDepth_ * kIndentStep);
    const uint16_t innerW =
        static_cast<uint16_t>(cfg_.pageWidth - cfg_.marginRight - effectiveLeft);
    const uint16_t imgX = (innerW > imgW)
                              ? static_cast<uint16_t>(effectiveLeft + (innerW - imgW) / 2)
                              : effectiveLeft;
    renderer_.drawImage(imgX, cursorY_, imgW, imgH, path, len);
  }
  cursorY_ = static_cast<uint16_t>(cursorY_ + imgH);
  // After image, force a line break so subsequent text starts on a
  // fresh line below.  Also re-check pageFull.
  if (cursorY_ + currentLineHeight() > cfg_.pageHeight - cfg_.marginBottom) {
    pageFull_ = true;
  }
  return pageFull_ ? ObserverStatus::Stop : ObserverStatus::Continue;
}

ObserverStatus StreamingPaginator::onStreamEnd() {
  // v2.0.133 — flush partial word too so the final word of the chapter
  // gets drawn even if the source bytes didn't end with whitespace.
  flushPartWord();
  // Flush whatever's left so the last partial line gets drawn.
  if (lineWordCount_ > 0 && !pageFull_) {
    flushLine();
  }
  return ObserverStatus::Continue;
}

// =============================================================================
// Internal helpers
// =============================================================================

bool StreamingPaginator::tryAddWord(const uint8_t* word, size_t len, uint32_t srcOffset) {
  if (len == 0) return true;

  // v2.0.135 — consume the inline-style-continuation flag (one-shot).
  // When set, this word glues to the previous slot with NO inter-word
  // space (e.g., the "ро" from HTML `хо<b>ро</b>шо` continues "хо"
  // without a visible space).  See doc on `partWordContinuation_`.
  const bool joinedToPrevious = partWordContinuation_;
  partWordContinuation_ = false;

  const uint16_t wordWidth = renderer_.measureWidth(word, len, styleBits_);
  const uint16_t spaceWidth = renderer_.getSpaceWidth(styleBits_);
  const uint16_t pageRightEdge = static_cast<uint16_t>(cfg_.pageWidth - cfg_.marginRight);
  const bool addLeadingSpace = lineHasContent_ && !joinedToPrevious;
  const uint16_t leadingSpaceWidth = addLeadingSpace ? spaceWidth : 0;
  // v2.0.145 — if this is the FIRST word on the line, cursorX_ may be
  // stale (pointing at the previous line's left margin without the new
  // indent applied).  Sync to the effective left margin so indent
  // changes take effect at the start of each fresh line.
  if (!lineHasContent_) {
    cursorX_ = static_cast<uint16_t>(cfg_.marginLeft + indentDepth_ * kIndentStep);
    // v3.1.0 — "красная строка": indent the first line of a top-level prose
    // paragraph.  Skip when centered/heading (own alignment) or inside a
    // list/quote block (indentDepth_>0, already offset).  Consume the flag
    // here so wrapped continuation lines of the same paragraph stay flush.
    if (atParagraphStart_) {
      if (cfg_.firstLineIndent > 0 && !centered_ && indentDepth_ == 0 &&
          (styleBits_ & kStyleHeading) == 0) {
        cursorX_ = static_cast<uint16_t>(cursorX_ + cfg_.firstLineIndent);
        lineIndent_ = cfg_.firstLineIndent;  // flushLine draws from here
      }
      atParagraphStart_ = false;
    }
  }
  const uint16_t lineWidthIfAdded = static_cast<uint16_t>(
      cursorX_ + leadingSpaceWidth + wordWidth);

  // Case A: word fits on the current line — accept and add.
  if (lineWidthIfAdded <= pageRightEdge && lineTextLen_ + len <= kLineTextCapacity &&
      lineWordCount_ < kLineWordCapacity) {
    // Append text to line buffer.
    const uint16_t off = lineTextLen_;
    std::memcpy(lineText_ + off, word, len);
    lineTextLen_ = static_cast<uint16_t>(off + len);

    // Capture word slot.
    WordSlot& slot = lineWords_[lineWordCount_++];
    slot.textOffset = off;
    slot.textLen = static_cast<uint16_t>(len);
    slot.pixelWidth = wordWidth;
    slot.styleBits = styleBits_;
    slot.noLeadingSpace = !addLeadingSpace;

    // Advance cursor on the line: the actual draw happens at flush time,
    // but cursorX is updated so subsequent fit-checks are accurate.
    cursorX_ = static_cast<uint16_t>(cursorX_ + leadingSpaceWidth + wordWidth);
    lineHasContent_ = true;
    return true;
  }

  // Case B: word doesn't fit on current line.  Flush what's there, advance
  // to next line, and try to put the word at the start of the new line.
  // If line buffer is empty (single oversized word case), we let it through
  // anyway — clipping is the renderer's problem; better than infinite loop.
  // v2.0.142 — pass justify=true because this line wrapped due to the
  // next word not fitting (so it's NOT a paragraph-end and should be
  // full-justified to fill out to the right margin).
  if (lineWordCount_ > 0) {
    flushLine(/*justify=*/true);
    if (pageFull_) {
      // Page filled by the flush — stash the word for the next page.
      if (len <= sizeof(carryWordText_)) {
        std::memcpy(carryWordText_, word, len);
        carryWordLen_ = static_cast<uint16_t>(len);
        carryWordWidth_ = wordWidth;
        carryWordStyle_ = styleBits_;
        // v2.0.140 — record where the carry word begins in source.
        // PageCountingObserver reads this for the `.idx` byteOffset.
        carryStartByteOffset_ = srcOffset;
      } else {
        // Too big to carry — drop.  R3 hyphenation will handle this case
        // by splitting the word; until then, words >64 bytes are rare in
        // typical text and dropping them is preferable to OOM.
        carryWordLen_ = 0;
        carryStartByteOffset_ = 0;
      }
      return false;
    }
    // Recurse once: now we have an empty fresh line, try to add the word.
    // v2.0.135: the recursive call will see lineHasContent_=false, so
    // `addLeadingSpace` is false regardless of `joinedToPrevious` — line-
    // wrap is a hard break, continuation doesn't apply across the wrap.
    // v2.0.140: propagate srcOffset so the carry path of the inner call
    // gets the right source position.
    return tryAddWord(word, len, srcOffset);
  }

  // Case C: word doesn't fit even on an empty line (oversized for column).
  // Just accept it; renderer clips.  Real hyphenation comes in R3.
  if (lineTextLen_ + len <= kLineTextCapacity && lineWordCount_ < kLineWordCapacity) {
    const uint16_t off = lineTextLen_;
    std::memcpy(lineText_ + off, word, len);
    lineTextLen_ = static_cast<uint16_t>(off + len);
    WordSlot& slot = lineWords_[lineWordCount_++];
    slot.textOffset = off;
    slot.textLen = static_cast<uint16_t>(len);
    slot.pixelWidth = wordWidth;
    slot.styleBits = styleBits_;
    slot.noLeadingSpace = !addLeadingSpace;
    cursorX_ = static_cast<uint16_t>(cursorX_ + wordWidth);
    lineHasContent_ = true;
    return true;
  }

  // Buffer overflow on a single oversized word — flush as-is.
  flushLine();
  return !pageFull_;
}

void StreamingPaginator::flushLine(bool justify) {
  // v2.0.145 — effective left margin includes the indent step × depth.
  const uint16_t effectiveLeft =
      static_cast<uint16_t>(cfg_.marginLeft + indentDepth_ * kIndentStep);

  if (lineWordCount_ == 0 && !lineHasContent_) {
    // Empty line — just advance cursor by one line height (e.g. for a
    // <br/> at start of paragraph).  Still might trigger page-full.
    cursorY_ = static_cast<uint16_t>(cursorY_ + currentLineHeight());
    cursorX_ = effectiveLeft;
    lineIndent_ = 0;  // v3.1.0
    if (cursorY_ + currentLineHeight() > cfg_.pageHeight - cfg_.marginBottom) {
      pageFull_ = true;
    }
    return;
  }

  // Draw each word at its computed x position.  Words were inserted in
  // order, so we re-walk and accumulate x with space gaps between them.
  // v2.0.121 R3.4: skip the actual drawWord calls when MEASURE-only mode
  // is active (drawSuppressed_).  Page-fullness logic continues to run
  // because line+cursor advancement happens below this loop regardless.
  // v2.0.135 R3.4: honour `slot.noLeadingSpace` so consecutive slots from
  // a single source word with inline style toggles (e.g. `хо<b>ро</b>шо`)
  // glue together without spurious inter-word spaces.
  const uint16_t spaceWidth = renderer_.getSpaceWidth(styleBits_);

  // v2.0.145 — center mode: NEVER justify; instead push the start x in
  // by half of the slack so the line is horizontally centered within
  // the indented working area.
  if (centered_) justify = false;

  // v2.0.142 — full-justification (spread inter-word gaps to reach the
  // right margin).  ONLY for lines that wrapped due to next-word-doesn't-
  // fit (justify=true) AND have more than one word AND aren't headings.
  // Last line of a paragraph (explicit break) stays left-aligned per
  // typographic convention.  Centered lines never justify.
  uint16_t extraSpacePerGap = 0;
  uint16_t extraSpaceRemainder = 0;
  uint16_t numGaps = 0;
  if (justify && lineWordCount_ > 1 && (styleBits_ & kStyleHeading) == 0) {
    // Count gaps (slots with leading space — skip noLeadingSpace
    // continuations so we don't visually break a styled-mid-word group
    // like `хо<b>ро</b>шо`).
    for (uint16_t i = 1; i < lineWordCount_; ++i) {
      if (!lineWords_[i].noLeadingSpace) ++numGaps;
    }
    if (numGaps > 0) {
      const uint16_t rightEdge =
          static_cast<uint16_t>(cfg_.pageWidth - cfg_.marginRight);
      // cursorX_ already tracks the natural line end (last word's right
      // edge given min-space gaps), so slack = rightEdge - cursorX_.
      if (cursorX_ < rightEdge) {
        const uint16_t slack = static_cast<uint16_t>(rightEdge - cursorX_);
        // Bound the per-gap stretch so we don't blow up lines with very
        // few words and tons of slack into "spaced  out  like  this".
        // Cap at 3x the natural space width.
        const uint16_t maxStretchPerGap = static_cast<uint16_t>(spaceWidth * 3);
        uint16_t stretch = slack;
        if (stretch / numGaps > maxStretchPerGap) {
          stretch = static_cast<uint16_t>(maxStretchPerGap * numGaps);
        }
        extraSpacePerGap = static_cast<uint16_t>(stretch / numGaps);
        extraSpaceRemainder = static_cast<uint16_t>(stretch % numGaps);
      }
    }
  }

  // v2.0.145 — center mode: compute start x = effectiveLeft + half-slack.
  // v3.1.0 — non-centered lines start at effectiveLeft + lineIndent_ so the
  // "красная строка" first line of a paragraph is offset (lineIndent_ is 0
  // for continuation lines and always 0 when centered_).
  uint16_t x = static_cast<uint16_t>(effectiveLeft + lineIndent_);
  if (centered_) {
    const uint16_t rightEdge =
        static_cast<uint16_t>(cfg_.pageWidth - cfg_.marginRight);
    if (cursorX_ < rightEdge) {
      const uint16_t slack = static_cast<uint16_t>(rightEdge - cursorX_);
      x = static_cast<uint16_t>(effectiveLeft + slack / 2);
    }
  }

  uint16_t gapsSeen = 0;
  for (uint16_t i = 0; i < lineWordCount_; ++i) {
    const WordSlot& slot = lineWords_[i];
    if (i > 0 && !slot.noLeadingSpace) {
      // Base inter-word space + justification extra (if any).
      // Remainder pixels (slack % numGaps) get distributed 1px each to
      // the FIRST `extraSpaceRemainder` gaps so the total exactly fills
      // the slack (no off-by-one truncation).
      uint16_t gap = static_cast<uint16_t>(spaceWidth + extraSpacePerGap);
      if (gapsSeen < extraSpaceRemainder) {
        gap = static_cast<uint16_t>(gap + 1);
      }
      ++gapsSeen;
      x = static_cast<uint16_t>(x + gap);
    }
    if (!drawSuppressed_) {
      renderer_.drawWord(x, cursorY_, lineText_ + slot.textOffset, slot.textLen, slot.styleBits);
    }
    x = static_cast<uint16_t>(x + slot.pixelWidth);
  }

  // Advance cursor past the drawn line.
  cursorY_ = static_cast<uint16_t>(cursorY_ + currentLineHeight());
  cursorX_ = effectiveLeft;
  lineTextLen_ = 0;
  lineWordCount_ = 0;
  lineHasContent_ = false;
  lineIndent_ = 0;  // v3.1.0 — consumed; continuation lines are flush-left

  // Page-full check: if the next line would start past the bottom margin,
  // we're done with this page.  Use the heading height conservatively in
  // case the next line is a heading.
  if (cursorY_ + currentLineHeight() > cfg_.pageHeight - cfg_.marginBottom) {
    pageFull_ = true;
  }
}

void StreamingPaginator::clearLine() {
  cursorX_ = cfg_.marginLeft;
  lineTextLen_ = 0;
  lineWordCount_ = 0;
  lineHasContent_ = false;
  lineIndent_ = 0;  // v3.1.0
}

void StreamingPaginator::saveSpillover(const uint8_t* text, size_t len, uint32_t srcOffset) {
  if (text == nullptr || len == 0) return;
  // v2.0.141 — record source offset of the FIRST saved byte; only on
  // the first save in a flush cycle (spillover empty before this call).
  // Subsequent saves within the same cycle just extend the existing
  // buffer; their source offset is implicitly contiguous so we leave
  // spilloverSourceOffset_ at the first-save value.
  if (spilloverLen_ == 0) {
    spilloverSourceOffset_ = srcOffset;
  }

  // v2.0.207 — GROW the heap buffer to fit instead of dropping the tail.
  // (Pre-fix this truncated at kSpilloverCapacity=2048, silently losing the
  // end of long page-straddling paragraphs — the "words missing between
  // pages" bug.)  Geometric growth, capped at kSpilloverMaxCapacity so the
  // 16-bit spilloverLen_ / pendingBytes() can't overflow.  resetForNextPage
  // may have moved the buffer out (null) — re-allocate lazily here.
  const size_t needed = static_cast<size_t>(spilloverLen_) + len;
  if (needed > spilloverCapacity_ && spilloverCapacity_ < kSpilloverMaxCapacity) {
    size_t newCap = spilloverCapacity_ ? spilloverCapacity_ : kSpilloverCapacity;
    while (newCap < needed && newCap < kSpilloverMaxCapacity) {
      newCap *= 2;
    }
    if (newCap > kSpilloverMaxCapacity) newCap = kSpilloverMaxCapacity;
    auto grown = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[newCap]);
    if (grown) {
      if (spilloverLen_ > 0 && spilloverBuf_) {
        std::memcpy(grown.get(), spilloverBuf_.get(), spilloverLen_);
      }
      spilloverBuf_ = std::move(grown);
      spilloverCapacity_ = newCap;
    }
    // Alloc failure → fall through and save what fits in the current buffer
    // (graceful degradation; still strictly better than the old fixed cap).
  }

  const size_t room = (spilloverCapacity_ > spilloverLen_)
                          ? (spilloverCapacity_ - spilloverLen_)
                          : 0;
  const size_t toSave = (len < room) ? len : room;
  if (toSave > 0 && spilloverBuf_) {
    std::memcpy(spilloverBuf_.get() + spilloverLen_, text, toSave);
    spilloverLen_ = static_cast<uint16_t>(spilloverLen_ + toSave);
  }
}

void StreamingPaginator::flushPartWord(bool setContinuation) {
  // v2.0.133 — submit any buffered partial word as a complete word.
  // Called from non-text event handlers (line break, style change, EOS)
  // to ensure a word being assembled across multiple onText calls is
  // committed before its style context shifts.  Idempotent when buffer
  // is empty.  Best-effort: if the word doesn't fit on the current line
  // AND the carry slot is occupied, tryAddWord's overflow handling kicks
  // in (typically: flush+retry, may set pageFull).  Return value of
  // tryAddWord is intentionally ignored — the caller has no clean way to
  // propagate Stop from inside e.g. onBoldStart.
  //
  // v2.0.135 — `setContinuation` raises `partWordContinuation_` AFTER
  // the partial is submitted, so the NEXT word goes into a slot with
  // `noLeadingSpace=true`.  Inline style toggles (e.g. `хо<b>ро</b>шо`)
  // produce a sequence of single-word text runs separated by event
  // handlers — without this flag the slots get inter-word spaces and
  // render as "хо ро шо" instead of "хорошо".  Pass `false` when the
  // caller knows the source had whitespace already (continuation case
  // in onText) or is reseting state (carry / page break).
  if (partWordLen_ == 0) return;
  const uint16_t len = partWordLen_;
  // v2.0.140 — capture partial start offset before clearing; this is
  // what carryStartByteOffset_ must take on if tryAddWord fails (the
  // dropped-bytes bug the partial-then-event-triggered-carry case
  // exhibited prior to v2.0.140).
  const uint32_t partialOffset = partialStartByteOffset_;
  partWordLen_ = 0;  // clear FIRST so any re-entrancy is safe
  partialStartByteOffset_ = 0;
  (void)tryAddWord(partWordBuf_, len, partialOffset);
  // tryAddWord consumed any prior continuation flag (one-shot).  Raise
  // it now so the NEXT tryAddWord call glues its slot to this one.
  if (setContinuation) {
    partWordContinuation_ = true;
  }
}

void StreamingPaginator::emitCarryWord() {
  if (carryWordLen_ == 0) return;
  // Re-add the carried word to the (now empty) line buffer.  We KNOW it
  // fits because either (a) it fit on a fresh line on the previous page
  // and was stashed only because we ran out of vertical room, or
  // (b) it was oversized and would also overflow here — in which case
  // tryAddWord falls into Case C and accepts it anyway.
  const uint16_t len = carryWordLen_;
  uint8_t buf[sizeof(carryWordText_)];
  std::memcpy(buf, carryWordText_, len);
  const uint8_t style = carryWordStyle_;
  // v2.0.140 — propagate source offset so a pathological re-carry on the
  // new page (e.g. carry word doesn't fit even on a fresh first line)
  // still records the correct `.idx` byteOffset.
  const uint32_t srcOffset = carryStartByteOffset_;
  carryWordLen_ = 0;
  carryStartByteOffset_ = 0;
  // Temporarily restore style for the carry word.
  const uint8_t savedStyle = styleBits_;
  styleBits_ = style;
  // v2.0.135 — carry word starts a fresh line on the new page; no
  // inline-style continuation context from the previous page applies.
  partWordContinuation_ = false;
  tryAddWord(buf, len, srcOffset);
  styleBits_ = savedStyle;
}

uint16_t StreamingPaginator::currentLineHeight() const {
  return (styleBits_ & kStyleHeading) ? cfg_.headingLineHeight : cfg_.bodyLineHeight;
}

uint16_t StreamingPaginator::verticalRemaining() const {
  const uint16_t bottom = static_cast<uint16_t>(cfg_.pageHeight - cfg_.marginBottom);
  if (cursorY_ >= bottom) return 0;
  return static_cast<uint16_t>(bottom - cursorY_);
}

}  // namespace snapix::smolport
