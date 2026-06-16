#include "test_utils.h"

#include <StreamingPaginator.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// =============================================================================
// StreamingPaginatorTest — proves the v3 event-driven page layouter
// (Phase R1 of the v3 architectural refactor) consumes a marker event
// stream and produces correct line/page break decisions WITHOUT building
// an intermediate Page/PageElement/TextBlock tree.
//
// Heap discipline (the whole point):
//   The paginator + this fake renderer must allocate ZERO bytes on the
//   heap during page layout.  All buffers are stack/BSS.  This test
//   doesn't enforce that with sandboxed allocator hooks (yet — Phase R3
//   when integrated against real GfxRenderer), but the API + struct
//   sizes are designed so a no-heap implementation is the natural one.
// =============================================================================

using snapix::smolport::PaginatorRenderer;
using snapix::smolport::StreamingPaginator;
using snapix::smolport::StreamingPaginatorConfig;
using snapix::smolport::ObserverStatus;
using snapix::smolport::kStyleBold;
using snapix::smolport::kStyleHeading;
using snapix::smolport::kStyleItalic;

// -----------------------------------------------------------------------------
// FakeRenderer — deterministic, zero-cost width metrics for assertions.
//
// Each ASCII letter = 6 px wide, regardless of style.  Space = 4 px.
// Heading scales x1.5 (so "h" = 9 px in heading mode, space = 6 px).
// drawWord() records the call — tests assert on emitted draw operations.
// -----------------------------------------------------------------------------
class FakeRenderer : public PaginatorRenderer {
 public:
  struct DrawCall {
    uint16_t x;
    uint16_t y;
    std::string text;
    uint8_t styleBits;
  };
  std::vector<DrawCall> drawCalls;

  uint16_t measureWidth(const uint8_t* text, size_t len, uint8_t styleBits) override {
    const uint16_t scale = (styleBits & kStyleHeading) ? 9 : 6;
    return static_cast<uint16_t>(scale * len);
  }
  uint16_t getSpaceWidth(uint8_t styleBits) override {
    return static_cast<uint16_t>((styleBits & kStyleHeading) ? 6 : 4);
  }
  void drawWord(uint16_t x, uint16_t y, const uint8_t* text, size_t len,
                uint8_t styleBits) override {
    DrawCall c;
    c.x = x;
    c.y = y;
    c.text = std::string(reinterpret_cast<const char*>(text), len);
    c.styleBits = styleBits;
    drawCalls.push_back(std::move(c));
  }
};

// -----------------------------------------------------------------------------
// Standard test config: 100×80 px page, 5 px margins, 12 px body line height.
// Working area: 90×70 px → ~5 lines fit per page.
// At 6 px/char + 4 px spaces, "abc def" = 18+4+18 = 40 px (fits), then
// " ghi" adds 4+18 = 22 px → total 62, still fits within 90 px.
// "jkl" pushes to 62+4+18 = 84 px (still fits).
// "mno" pushes to 84+4+18 = 106 px → wraps to next line.
// -----------------------------------------------------------------------------
static StreamingPaginatorConfig stdConfig() {
  StreamingPaginatorConfig cfg{};
  cfg.pageWidth = 100;
  cfg.pageHeight = 80;
  cfg.marginTop = 5;
  cfg.marginBottom = 5;
  cfg.marginLeft = 5;
  cfg.marginRight = 5;
  cfg.bodyLineHeight = 12;
  cfg.headingLineHeight = 18;
  cfg.paragraphSpacing = 4;
  return cfg;
}

static void feedText(StreamingPaginator& p, const std::string& s) {
  p.onText(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

int main() {
  TestUtils::TestRunner runner("StreamingPaginator");
  const auto cfg = stdConfig();

  // -----------------------------------------------------------------------
  // T1: empty stream produces no draw calls and isn't page-full.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    runner.expectFalse(p.isPageFull(), "empty_not_full");
    runner.expectEq(size_t(0), fr.drawCalls.size(), "empty_no_draws");
    runner.expectEq(uint16_t(5), p.cursorY(), "empty_cursor_at_top_margin");
  }

  // -----------------------------------------------------------------------
  // T2: single short paragraph fits on first line.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "abc def");
    p.onParagraphBreak();
    // Two draw calls: "abc" at x=5, "def" at x=5+18+4=27.
    runner.expectEq(size_t(2), fr.drawCalls.size(), "T2_two_words_drawn");
    runner.expectEq(std::string("abc"), fr.drawCalls[0].text, "T2_first_word_text");
    runner.expectEq(uint16_t(5), fr.drawCalls[0].x, "T2_first_word_x");
    runner.expectEq(std::string("def"), fr.drawCalls[1].text, "T2_second_word_text");
    runner.expectEq(uint16_t(27), fr.drawCalls[1].x, "T2_second_word_x");
    runner.expectFalse(p.isPageFull(), "T2_not_page_full");
  }

  // -----------------------------------------------------------------------
  // T3: line wraps when next word would exceed right margin.
  //   "abc def ghi jkl mno" — first four fit (84 px), "mno" wraps.
  //   After flush of first line: cursorY advanced by bodyLineHeight (12).
  //   "mno" lands on line 2 at x=marginLeft=5.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "abc def ghi jkl mno");
    p.onStreamEnd();
    runner.expectEq(size_t(5), fr.drawCalls.size(), "T3_five_words_drawn");
    // First four on line 1 (y=5):
    runner.expectEq(uint16_t(5), fr.drawCalls[0].y, "T3_word0_line1");
    runner.expectEq(uint16_t(5), fr.drawCalls[3].y, "T3_word3_line1");
    // Fifth on line 2 (y=5+12=17):
    runner.expectEq(uint16_t(17), fr.drawCalls[4].y, "T3_word4_line2");
    runner.expectEq(std::string("mno"), fr.drawCalls[4].text, "T3_word4_text");
    runner.expectEq(uint16_t(5), fr.drawCalls[4].x, "T3_word4_x_at_left_margin");
  }

  // -----------------------------------------------------------------------
  // T4: explicit page break stops the stream.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "abc def");
    runner.expectFalse(p.isPageFull(), "T4_pre_break_not_full");
    auto status = p.onPageBreak();
    runner.expectTrue(status == ObserverStatus::Stop, "T4_break_signals_stop");
    runner.expectTrue(p.isPageFull(), "T4_post_break_full");
  }

  // -----------------------------------------------------------------------
  // T5: vertical overflow triggers page-full.
  //   Working area = (80 - 5 - 5) = 70 px tall.
  //   Each body line = 12 px.  Lines that fit: 5 (taking 60 px), 6th would
  //   need cursorY=72; we'd then check cursorY+12=84 > 75 → page full.
  //   Use single-word lines via paragraph breaks to count lines easily.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    int wordsAdded = 0;
    for (int i = 0; i < 20 && !p.isPageFull(); ++i) {
      feedText(p, "x");
      p.onParagraphBreak();
      ++wordsAdded;
    }
    runner.expectTrue(p.isPageFull(), "T5_eventually_page_full");
    // Should have drawn ~5 words on ~5 lines before filling.
    runner.expectTrue(fr.drawCalls.size() >= 4 && fr.drawCalls.size() <= 6,
                      "T5_drew_about_5_lines");
  }

  // -----------------------------------------------------------------------
  // T6: style bits propagate to drawWord calls.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "plain ");
    p.onBoldStart();
    feedText(p, "bold ");
    p.onBoldEnd();
    p.onItalicStart();
    feedText(p, "italic");
    p.onItalicEnd();
    p.onStreamEnd();
    runner.expectEq(size_t(3), fr.drawCalls.size(), "T6_three_words");
    runner.expectEq(uint8_t(0), fr.drawCalls[0].styleBits, "T6_w0_plain");
    runner.expectEq(uint8_t(kStyleBold), fr.drawCalls[1].styleBits, "T6_w1_bold");
    runner.expectEq(uint8_t(kStyleItalic), fr.drawCalls[2].styleBits, "T6_w2_italic");
  }

  // -----------------------------------------------------------------------
  // T7: paragraph break advances cursor by lineHeight + paragraphSpacing.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "first");
    p.onParagraphBreak();
    feedText(p, "second");
    p.onStreamEnd();
    runner.expectEq(size_t(2), fr.drawCalls.size(), "T7_two_words");
    runner.expectEq(uint16_t(5), fr.drawCalls[0].y, "T7_w0_line1");
    // Second word on line 2 + paragraph spacing: 5 + 12 + 4 = 21
    runner.expectEq(uint16_t(21), fr.drawCalls[1].y, "T7_w1_line2_with_para_space");
  }

  // -----------------------------------------------------------------------
  // T8: resetForNextPage clears page-full but keeps style state.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    p.onBoldStart();
    feedText(p, "bold");
    p.onPageBreak();
    runner.expectTrue(p.isPageFull(), "T8_full_after_break");
    p.resetForNextPage();
    runner.expectFalse(p.isPageFull(), "T8_not_full_after_reset");
    feedText(p, "still");
    p.onStreamEnd();
    // The "still" word should still be bold (style state preserved).
    runner.expectTrue(fr.drawCalls.size() >= 2, "T8_at_least_two_words_drawn");
    const auto& lastCall = fr.drawCalls.back();
    runner.expectTrue((lastCall.styleBits & kStyleBold) != 0, "T8_style_preserved_across_pages");
  }

  // -----------------------------------------------------------------------
  // T9: byte-alignment & UTF-8 — multi-byte chars don't crash measurement.
  //   Russian "Привет" = 12 UTF-8 bytes; renderer width = 6 * 12 = 72 px.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");  // "Привет"
    p.onStreamEnd();
    runner.expectEq(size_t(1), fr.drawCalls.size(), "T9_utf8_one_word");
    runner.expectEq(size_t(12), fr.drawCalls[0].text.size(), "T9_utf8_12_bytes");
  }

  // -----------------------------------------------------------------------
  // v2.0.133 — chunk-boundary word reassembly.
  //
  // PROBLEM (FB2 Russian text rendered as «пÿ°тегория», «вÿÿликие», etc.):
  //   MarkerStream calls onText once per text run, and chunked LittleFS
  //   reads can split a long text run into two onText calls right in the
  //   middle of a word — including the middle of a multi-byte UTF-8
  //   sequence.  Pre-fix, each onText was tokenized independently:
  //   trailing non-whitespace bytes became their own "word", and the
  //   leading bytes of the next onText became another "word" — producing
  //   ÿ/° latin-1 glyphs where the renderer hit invalid UTF-8 fragments.
  //
  //   Fix: partWordBuf_ accumulates trailing-non-whitespace bytes across
  //   onText calls, flushing only on whitespace OR any structural event.
  // -----------------------------------------------------------------------

  // T10: a word split mid-stream by two onText calls produces ONE drawn word.
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    // "hello world" split as "hel" + "lo world" — chunk boundary inside "hello".
    feedText(p, "hel");
    feedText(p, "lo world");
    p.onStreamEnd();
    runner.expectEq(size_t(2), fr.drawCalls.size(), "T10_split_word_two_drawn_words");
    runner.expectEq(std::string("hello"), fr.drawCalls[0].text, "T10_split_word_reassembled");
    runner.expectEq(std::string("world"), fr.drawCalls[1].text, "T10_second_word_intact");
  }

  // T11: UTF-8 multi-byte char split across two onText calls.
  // "Привет" = D0 9F D1 80 D0 B8 D0 B2 D0 B5 D1 82 (12 bytes).
  // Split between byte 5 (0xB8 lead of 'и') and byte 6 (0xD0 lead of 'в'):
  // wait, the danger is splitting WITHIN a code-point, e.g. after D0 of 'в'.
  // Use a split mid-codepoint: 7 bytes ("Прив" + lead-byte of 'е') | rest.
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    // First chunk: "Прив" (8 bytes) + 0xD0 (lead byte of 'е').
    p.onText(reinterpret_cast<const uint8_t*>("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0"), 9);
    // Second chunk: 0xB5 (continuation byte) + "т" (D1 82) — completes "Привет".
    p.onText(reinterpret_cast<const uint8_t*>("\xB5\xD1\x82"), 3);
    p.onStreamEnd();
    runner.expectEq(size_t(1), fr.drawCalls.size(), "T11_utf8_split_one_word");
    runner.expectEq(size_t(12), fr.drawCalls[0].text.size(), "T11_utf8_split_12_bytes_reassembled");
    // Bytes must match the original "Привет" exactly — no ÿ/° corruption.
    const uint8_t kExpected[] = {0xD0, 0x9F, 0xD1, 0x80, 0xD0, 0xB8, 0xD0, 0xB2, 0xD0, 0xB5, 0xD1, 0x82};
    runner.expectTrue(std::memcmp(fr.drawCalls[0].text.data(), kExpected, 12) == 0,
                      "T11_utf8_bytes_bit_for_bit");
  }

  // T12: word split across THREE onText calls.
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "ca");
    feedText(p, "te");
    feedText(p, "g word");
    p.onStreamEnd();
    runner.expectEq(size_t(2), fr.drawCalls.size(), "T12_three_chunks_two_words");
    runner.expectEq(std::string("categ"), fr.drawCalls[0].text, "T12_full_word_assembled");
    runner.expectEq(std::string("word"), fr.drawCalls[1].text, "T12_separator_word_intact");
  }

  // T13: structural event (line break) flushes partial word.
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "hel");      // partial: "hel"
    p.onLineBreak();         // flush partial → submit "hel"
    feedText(p, "lo world");
    p.onStreamEnd();
    runner.expectEq(size_t(3), fr.drawCalls.size(), "T13_linebreak_flushes_partial");
    runner.expectEq(std::string("hel"), fr.drawCalls[0].text, "T13_partial_was_flushed");
    runner.expectEq(std::string("lo"), fr.drawCalls[1].text, "T13_post_break_first_word");
    runner.expectEq(std::string("world"), fr.drawCalls[2].text, "T13_post_break_second_word");
  }

  // T14: style change in the middle of a buffered partial word commits the
  // partial under the OLD style (so the second half won't be styled wrong).
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "pre");      // partial: "pre", style=plain
    p.onBoldStart();         // flush partial as plain, then bold ON
    feedText(p, "post text");
    p.onStreamEnd();
    runner.expectEq(size_t(3), fr.drawCalls.size(), "T14_bold_flushes_partial");
    runner.expectEq(std::string("pre"), fr.drawCalls[0].text, "T14_partial_text");
    runner.expectEq(uint8_t(0), fr.drawCalls[0].styleBits, "T14_partial_old_style_plain");
    runner.expectEq(std::string("post"), fr.drawCalls[1].text, "T14_post_text");
    runner.expectTrue((fr.drawCalls[1].styleBits & kStyleBold) != 0, "T14_post_style_bold");
  }

  // T15: chapter reset clears partial-word state (no leakage between chapters).
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "leak");     // partial: "leak"
    p.resetForChapter();
    feedText(p, "fresh other");
    p.onStreamEnd();
    runner.expectEq(size_t(2), fr.drawCalls.size(), "T15_reset_drops_partial");
    runner.expectEq(std::string("fresh"), fr.drawCalls[0].text, "T15_no_leak_prefix");
    runner.expectEq(std::string("other"), fr.drawCalls[1].text, "T15_clean_chapter_start");
  }

  // T16: end-of-stream flushes partial word (last word in chapter has no
  // trailing whitespace).
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "alpha bet");  // partial: "bet" (no trailing whitespace)
    p.onStreamEnd();            // should flush "bet"
    runner.expectEq(size_t(2), fr.drawCalls.size(), "T16_eos_flushes_partial");
    runner.expectEq(std::string("alpha"), fr.drawCalls[0].text, "T16_first_word");
    runner.expectEq(std::string("bet"), fr.drawCalls[1].text, "T16_eos_flushed_last");
  }

  // -----------------------------------------------------------------------
  // v2.0.135 — inline-style-toggle glue.
  //
  // PROBLEM (after v2.0.133's partial-word buffer):
  //   HTML `хо<b>ро</b>шо` strips to three text runs ("хо", "ро", "шо")
  //   separated by onBoldStart/onBoldEnd.  Each run was buffered as a
  //   partial and committed by the style-change handler's flushPartWord
  //   as a separate WordSlot.  flushLine inserted spaceWidth between
  //   every adjacent pair → visible "хо ро шо".  In Russian books with
  //   frequent inline emphasis, accumulated spurious spaces made lines
  //   wrap at half the screen width.
  //
  // FIX:
  //   flushPartWord(setContinuation=true) raises partWordContinuation_;
  //   tryAddWord consumes it and marks the new slot's noLeadingSpace.
  //   flushLine skips spaceWidth for slots flagged that way.
  //   The flag clears on (a) one-shot consume in tryAddWord, (b) leading
  //   whitespace at start of onText, (c) hard breaks (page/chapter reset).
  // -----------------------------------------------------------------------

  // T17: word-mid-style-toggle glues with no inter-word space.
  //   HTML "abc<b>def</b>ghi" → 3 slots [abc, def, ghi].  Render expects
  //   slot1 (def) starts exactly where slot0 (abc) ends, no spaceWidth
  //   gap.  Slot2 (ghi) starts exactly where slot1 (def) ends.
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "abc");      // partial="abc"
    p.onBoldStart();         // flushPartWord → submit "abc". slot[0]=abc.
    feedText(p, "def");      // partial="def"
    p.onBoldEnd();           // flushPartWord → submit "def". slot[1]=def joined.
    feedText(p, "ghi");      // partial="ghi"
    p.onStreamEnd();         // flushPartWord → submit "ghi". slot[2]=ghi joined.

    runner.expectEq(size_t(3), fr.drawCalls.size(), "T17_three_slots");
    runner.expectEq(std::string("abc"), fr.drawCalls[0].text, "T17_slot0_abc");
    runner.expectEq(std::string("def"), fr.drawCalls[1].text, "T17_slot1_def");
    runner.expectEq(std::string("ghi"), fr.drawCalls[2].text, "T17_slot2_ghi");
    // 6 px/char.  abc=18 px starting at marginLeft (5) → ends at x=23.
    // def MUST start at x=23 (NO leading spaceWidth=4), ends at 41.
    // ghi MUST start at x=41 (NO leading space), ends at 59.
    runner.expectEq(uint16_t(5), fr.drawCalls[0].x, "T17_abc_x");
    runner.expectEq(uint16_t(5 + 18), fr.drawCalls[1].x, "T17_def_glued_no_space");
    runner.expectEq(uint16_t(5 + 18 + 18), fr.drawCalls[2].x, "T17_ghi_glued_no_space");
  }

  // T18: word-then-space-then-style — preserves the inter-word space.
  //   HTML "abc <b>def</b>ghi" → "abc" via main loop (space after),
  //   then "def" via flushPartWord (no space before since main-loop
  //   submit didn't raise the flag), then "ghi" glued to "def".
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "abc ");     // submits "abc" via main loop, partWordLen_=0
    p.onBoldStart();         // flushPartWord no-op (partial empty)
    feedText(p, "def");      // partial="def"
    p.onBoldEnd();           // flushPartWord submits "def" — flag was false → space added
    feedText(p, "ghi");      // partial="ghi"
    p.onStreamEnd();         // flushPartWord submits "ghi" — flag from prev flush → glued

    runner.expectEq(size_t(3), fr.drawCalls.size(), "T18_three_slots");
    runner.expectEq(uint16_t(5), fr.drawCalls[0].x, "T18_abc_x");
    // def has leading space: x = 5 + 18 + 4(spaceWidth) = 27
    runner.expectEq(uint16_t(5 + 18 + 4), fr.drawCalls[1].x, "T18_def_with_space");
    // ghi glued: x = 27 + 18 = 45
    runner.expectEq(uint16_t(5 + 18 + 4 + 18), fr.drawCalls[2].x, "T18_ghi_glued");
  }

  // T19: leading whitespace in next onText clears continuation flag.
  //   HTML "abc<b>def</b> ghi" → "abc" buffered, "def" flushed glued,
  //   then " ghi" with leading space clears flag → "ghi" with space.
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "abc");      // partial="abc"
    p.onBoldStart();         // submit "abc". flag=true.
    feedText(p, "def");      // partial="def". flag=true.
    p.onBoldEnd();           // submit "def" glued. flag=true.
    feedText(p, " ghi");     // leading space clears flag. Find "ghi" at end → buffer.
    p.onStreamEnd();         // submit "ghi". flag=false → space added.

    runner.expectEq(size_t(3), fr.drawCalls.size(), "T19_three_slots");
    runner.expectEq(uint16_t(5), fr.drawCalls[0].x, "T19_abc_x");
    runner.expectEq(uint16_t(5 + 18), fr.drawCalls[1].x, "T19_def_glued");
    // ghi after explicit space: 5 + 18 + 18 + 4 = 45
    runner.expectEq(uint16_t(5 + 18 + 18 + 4), fr.drawCalls[2].x, "T19_ghi_after_space");
  }

  // T20: italic + bold mid-word.  "хо<i>ро</i><b>ши</b>е" → 4 slots all
  // glued (no spaces between any of them).
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "ho");
    p.onItalicStart();
    feedText(p, "ro");
    p.onItalicEnd();
    p.onBoldStart();
    feedText(p, "sh");
    p.onBoldEnd();
    feedText(p, "ie");
    p.onStreamEnd();

    runner.expectEq(size_t(4), fr.drawCalls.size(), "T20_four_slots");
    // ho=12, ro=12 glued at 5+12=17, sh=12 glued at 17+12=29, ie=12 at 29+12=41
    runner.expectEq(uint16_t(5),      fr.drawCalls[0].x, "T20_ho_x");
    runner.expectEq(uint16_t(5 + 12), fr.drawCalls[1].x, "T20_ro_glued");
    runner.expectEq(uint16_t(29),     fr.drawCalls[2].x, "T20_sh_glued");
    runner.expectEq(uint16_t(41),     fr.drawCalls[3].x, "T20_ie_glued");
  }

  // T21: chunk-boundary word-split (T11 scenario) MUST NOT introduce a
  // spurious space either.  Chunk1 "abc", chunk2 "def" with no events
  // between → single assembled word "abcdef".
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "abc");      // partial="abc"
    feedText(p, "def");      // continuation: append "def" → partial="abcdef"
    p.onStreamEnd();         // flush "abcdef" as one word

    runner.expectEq(size_t(1), fr.drawCalls.size(), "T21_one_word_reassembled");
    runner.expectEq(std::string("abcdef"), fr.drawCalls[0].text, "T21_word_text");
  }

  // T22: line break between styled fragments — line break is a hard
  // separator, the SECOND line's words start fresh at left margin.
  {
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "abc");
    p.onBoldStart();
    feedText(p, "def");
    p.onLineBreak();          // hard break — flushes partial, flushes line
    feedText(p, "ghi");
    p.onStreamEnd();

    runner.expectEq(size_t(3), fr.drawCalls.size(), "T22_three_slots_two_lines");
    // Line 1: abc + def glued
    runner.expectEq(uint16_t(5),      fr.drawCalls[0].x, "T22_abc_x");
    runner.expectEq(uint16_t(5 + 18), fr.drawCalls[1].x, "T22_def_glued");
    // Line 2: ghi at left margin
    runner.expectEq(uint16_t(5), fr.drawCalls[2].x, "T22_ghi_new_line");
  }

  // -----------------------------------------------------------------------
  // v2.0.137 — text spillover on page-full at mid-text-run.
  //
  // PROBLEM:
  //   `MarkerStream::feed` advances past the entire text run before
  //   calling `observer.onText`.  When `tryAddWord` returns false
  //   (pageFull from carry), the bytes AFTER the carried word in the
  //   same `onText` call get DROPPED.  Pre-fix: every page boundary
  //   silently dropped content.  Visible as "pages don't connect"
  //   in the rendered output.
  //
  // FIX:
  //   Save the trailing bytes into `spilloverBuf_` on pageFull.
  //   `resetForNextPage` runs them through the text path after
  //   `emitCarryWord` so the carry word + spillover render in source
  //   order on the new page.
  // -----------------------------------------------------------------------

  // T23: text exceeds page capacity, pageFull triggers; the carry word
  //   AND all subsequent words in the same onText call should
  //   eventually render (via spillover replay across pages), in source
  //   order, no drops.
  {
    StreamingPaginatorConfig narrow = cfg;
    narrow.pageHeight = 30;  // ~one line fits (margins+line=22)
    FakeRenderer fr;
    StreamingPaginator p(narrow, fr);
    feedText(p, "aa bb cc dd ee ff gg hh ii jj kk ll mm nn oo pp qq rr ss tt uu vv");
    runner.expectTrue(p.isPageFull(), "T23_page_filled");

    // Drain pages until spillover is empty (no more pending bytes).
    // Safety cap of 30 iterations to avoid infinite loop on bug.
    for (int iter = 0; iter < 30; ++iter) {
      p.resetForNextPage();
      if (p.pendingBytes() == 0 && !p.isPageFull()) break;
    }
    p.onStreamEnd();  // flush trailing line

    // All 22 words should have been drawn in source order across the pages.
    std::string sequence;
    for (size_t i = 0; i < fr.drawCalls.size(); ++i) {
      if (i > 0) sequence += ' ';
      sequence += fr.drawCalls[i].text;
    }
    // Spot-check that the full sequence is preserved.
    runner.expectTrue(sequence.find("aa bb cc dd ee ff gg hh") != std::string::npos,
                      "T23_first_words_in_order");
    runner.expectTrue(sequence.find("ss tt uu vv") != std::string::npos,
                      "T23_last_words_present");
    // No spurious word duplication: count occurrences of "aa".
    size_t aaCount = 0;
    for (const auto& c : fr.drawCalls)
      if (c.text == "aa") ++aaCount;
    runner.expectEq(size_t(1), aaCount, "T23_no_duplicate_carry");
  }

  // T24: pageFull at top of onText preserves incoming text into spillover.
  //   Simulates the situation where MarkerStream feeds another text-run
  //   into a paginator that hasn't been reset yet.  In production the
  //   PageCountingObserver runs resetForNextPage between Stops; this
  //   test directly exercises the save-on-pageFull-at-top branch.
  {
    StreamingPaginatorConfig narrow = cfg;
    narrow.pageHeight = 30;
    FakeRenderer fr;
    StreamingPaginator p(narrow, fr);
    feedText(p, "aa bb cc dd ee ff gg hh ii jj kk ll mm nn oo pp qq rr ss tt uu vv");
    runner.expectTrue(p.isPageFull(), "T24_initial_page_full");

    // Capture pending bytes before second feed; should be > 0 (spillover
    // from the overflowing first feed).
    const uint16_t pendingBeforeSecond = p.pendingBytes();
    runner.expectTrue(pendingBeforeSecond > 0, "T24_pending_before_second_feed");

    // Feed extra text while still pageFull — bytes get appended to
    // spillover.  Pending count grows.
    feedText(p, " extra");  // leading space ensures clean word boundary
    runner.expectTrue(p.pendingBytes() > pendingBeforeSecond,
                      "T24_pending_grew_after_append");
  }

  // T25: pendingBytes() reports correct count after pageFull with carry+spillover.
  {
    StreamingPaginatorConfig narrow = cfg;
    narrow.pageHeight = 30;
    FakeRenderer fr;
    StreamingPaginator p(narrow, fr);
    feedText(p, "aa bb cc dd ee ff gg hh ii jj kk ll mm nn oo pp qq rr ss tt uu vv");
    runner.expectTrue(p.isPageFull(), "T25_page_filled");
    runner.expectTrue(p.pendingBytes() > 0,
                      "T25_pending_nonzero_after_pagefull");
  }

  // -----------------------------------------------------------------------
  // v2.0.142 — full-justification of wrapped lines (T26-T28).
  //
  // EXPECTED:
  //   * Multi-word line that wrapped (because next word didn't fit)
  //     is full-justified: last word's right edge ≈ rightMargin.
  //   * Last line of a paragraph (flushed via explicit paragraph break)
  //     stays left-aligned: last word's right edge < rightMargin.
  //   * Heading lines stay left-aligned even when wrapped.
  // -----------------------------------------------------------------------

  // T26: wrapped body line is justified out to right margin.
  {
    const auto cfg26 = stdConfig();
    FakeRenderer fr;
    StreamingPaginator p(cfg26, fr);
    // "abc def ghi jkl" = 4 words × 18px + 3 spaces × 4px = 84 px
    // (fits the 90 px working width).  Adding "mno" (18 px + 4 px space
    // = 22 px) overflows → wraps → line "abc def ghi jkl" justified.
    feedText(p, "abc def ghi jkl mno pqr");
    p.onStreamEnd();  // flush remaining (NOT justified)

    runner.expectTrue(fr.drawCalls.size() >= 4, "T26_at_least_four_draws");
    // Find the first 4 draws (= wrapped line "abc def ghi jkl").
    // Word 4 ("jkl") right edge should equal cfg.pageWidth - cfg.marginRight.
    const auto& jklCall = fr.drawCalls[3];  // 4th call
    const uint16_t jklWidth = 18;  // 3 chars × 6 px
    const uint16_t jklRight = static_cast<uint16_t>(jklCall.x + jklWidth);
    const uint16_t rightEdge =
        static_cast<uint16_t>(cfg26.pageWidth - cfg26.marginRight);
    runner.expectEq(jklRight, rightEdge,
                    "T26_wrapped_line_reaches_right_margin");
  }

  // T27: last line of paragraph stays left-aligned (NOT justified).
  {
    const auto cfg27 = stdConfig();
    FakeRenderer fr;
    StreamingPaginator p(cfg27, fr);
    feedText(p, "abc def ghi");
    p.onParagraphBreak();  // flush as paragraph end → NOT justified

    runner.expectEq(size_t(3), fr.drawCalls.size(), "T27_three_draws");
    // Right edge of "ghi" should NOT reach pageWidth-marginRight.
    const auto& ghiCall = fr.drawCalls[2];
    const uint16_t ghiWidth = 18;
    const uint16_t ghiRight = static_cast<uint16_t>(ghiCall.x + ghiWidth);
    const uint16_t rightEdge =
        static_cast<uint16_t>(cfg27.pageWidth - cfg27.marginRight);
    runner.expectTrue(ghiRight < rightEdge,
                      "T27_paragraph_last_line_ragged_right");
  }

  // T28: noLeadingSpace continuations (inline-style mid-word) do NOT
  //   receive justification extras — the visual word stays glued.
  {
    const auto cfg28 = stdConfig();
    FakeRenderer fr;
    StreamingPaginator p(cfg28, fr);
    // "хо" "ро" "шо" emulated via raw onText + style toggles.
    feedText(p, "hi ");
    p.onBoldStart();
    feedText(p, "ab");
    p.onBoldEnd();
    feedText(p, "cd ");
    p.onItalicStart();
    feedText(p, "ef");
    p.onItalicEnd();
    // Force a wrap by adding a long tail (so a previous line had to
    // flush via Case B with justify=true).
    feedText(p, "gggggggggg hhhhhhhhhh iiiiiiiiii jjjjjjjjjj kkkkkkkkkk");
    p.onStreamEnd();

    // Look for the "ab"+"cd" glued slots (no leading space between).
    // The "cd" slot should appear immediately right of "ab" — i.e.
    // distance between "ab".x + ab_width and "cd".x equals 0, regardless
    // of justification stretch.
    size_t abIdx = SIZE_MAX, cdIdx = SIZE_MAX;
    for (size_t i = 0; i < fr.drawCalls.size(); ++i) {
      if (fr.drawCalls[i].text == "ab") abIdx = i;
      if (fr.drawCalls[i].text == "cd") cdIdx = i;
    }
    if (abIdx != SIZE_MAX && cdIdx == abIdx + 1) {
      const uint16_t abWidth = 12;  // 2 chars × 6 px
      const uint16_t expectedCdX =
          static_cast<uint16_t>(fr.drawCalls[abIdx].x + abWidth);
      runner.expectEq(expectedCdX, fr.drawCalls[cdIdx].x,
                      "T28_continuation_glued_no_justify_extra");
    } else {
      runner.expectTrue(false, "T28_setup_ab_cd_adjacent");
    }
  }

  // -----------------------------------------------------------------------
  // v2.0.145 — Indent + Center (T29-T33).
  // -----------------------------------------------------------------------

  // T29: kIndentOn shifts left margin in by kIndentStep; words drawn
  //   at that new x position.
  {
    const auto cfg29 = stdConfig();
    FakeRenderer fr;
    StreamingPaginator p(cfg29, fr);
    p.onIndentStart();
    feedText(p, "abc");
    p.onStreamEnd();

    runner.expectEq(size_t(1), fr.drawCalls.size(), "T29_indent_one_draw");
    // First word should be drawn at marginLeft + kIndentStep (=5+24=29).
    if (!fr.drawCalls.empty()) {
      runner.expectEq(uint16_t(cfg29.marginLeft + 24),
                      fr.drawCalls[0].x, "T29_indent_x_shifted");
    }
  }

  // T30: nested indents (2 levels) double the shift.
  {
    const auto cfg30 = stdConfig();
    FakeRenderer fr;
    StreamingPaginator p(cfg30, fr);
    p.onIndentStart();
    p.onIndentStart();
    feedText(p, "abc");
    p.onStreamEnd();

    if (!fr.drawCalls.empty()) {
      runner.expectEq(uint16_t(cfg30.marginLeft + 48),
                      fr.drawCalls[0].x, "T30_double_indent_x");
    }
  }

  // T31: kIndentOff pops one level; word goes back to single indent.
  {
    const auto cfg31 = stdConfig();
    FakeRenderer fr;
    StreamingPaginator p(cfg31, fr);
    p.onIndentStart();
    p.onIndentStart();
    p.onIndentEnd();
    feedText(p, "abc");
    p.onStreamEnd();

    if (!fr.drawCalls.empty()) {
      runner.expectEq(uint16_t(cfg31.marginLeft + 24),
                      fr.drawCalls[0].x, "T31_indent_pop_back_one_level");
    }
  }

  // T32: kCenterOn centers the line within the working width.
  {
    const auto cfg32 = stdConfig();
    FakeRenderer fr;
    StreamingPaginator p(cfg32, fr);
    p.onCenterStart();
    feedText(p, "abc");
    p.onStreamEnd();

    runner.expectEq(size_t(1), fr.drawCalls.size(), "T32_center_one_draw");
    if (!fr.drawCalls.empty()) {
      // "abc" = 3 chars × 6 px = 18 px wide.  Working width =
      // 100-5-5 = 90 px.  Centered x = marginLeft + (90 - 18) / 2 =
      // 5 + 36 = 41.
      runner.expectEq(uint16_t(41), fr.drawCalls[0].x,
                      "T32_center_x_correct");
    }
  }

  // T33: center doesn't justify even when line would normally wrap-
  //   justify.  Multi-word centered line stays centered, no stretch.
  {
    const auto cfg33 = stdConfig();
    FakeRenderer fr;
    StreamingPaginator p(cfg33, fr);
    p.onCenterStart();
    // "abc def" = 18+4+18 = 40 px, fits and isn't wide enough to wrap.
    feedText(p, "abc def");
    p.onCenterEnd();
    p.onStreamEnd();

    runner.expectEq(size_t(2), fr.drawCalls.size(), "T33_center_two_draws");
    // First word's leading: (90 - 40) / 2 = 25, so x = 5+25=30.
    // Second word's x = 30 + 18 + 4 = 52.
    if (fr.drawCalls.size() >= 2) {
      runner.expectEq(uint16_t(30), fr.drawCalls[0].x, "T33_center_w1_x");
      runner.expectEq(uint16_t(52), fr.drawCalls[1].x, "T33_center_w2_x_no_justify");
    }
  }

  // -----------------------------------------------------------------------
  // v2.0.147 — kPageBreak at chapter start is no-op (empty-page fix).
  //
  // FB2 sources emit kPageBreak on `<section>` open, which is the very
  // first event of every chapter.  Pre-fix, that immediately set
  // pageFull → checkAdvance fired boundary for page 1 → page 0 was
  // an empty page (no content ever drawn on it).  User-visible:
  // every freshly-indexed FB2 chapter showed an empty page 0.
  // -----------------------------------------------------------------------

  // T34: pageBreak as first event of chapter does NOT trigger pageFull.
  {
    const auto cfg34 = stdConfig();
    FakeRenderer fr;
    StreamingPaginator p(cfg34, fr);
    p.onPageBreak();
    runner.expectFalse(p.isPageFull(), "T34_page_break_at_start_no_pagefull");
    feedText(p, "first content");
    p.onStreamEnd();
    runner.expectTrue(!fr.drawCalls.empty(),
                      "T34_content_after_initial_page_break_drawn");
  }

  // T35: pageBreak after content DOES trigger pageFull (normal behaviour).
  {
    const auto cfg35 = stdConfig();
    FakeRenderer fr;
    StreamingPaginator p(cfg35, fr);
    feedText(p, "first content");
    p.onPageBreak();
    runner.expectTrue(p.isPageFull(),
                      "T35_page_break_after_content_pagefull");
  }

  // -----------------------------------------------------------------------
  // v2.0.207 — REGRESSION: a long single text-run (one marker-free
  // paragraph) whose post-carry tail exceeds the old 2048-byte spillover
  // cap must NOT drop words.  Pre-fix, saveSpillover() truncated the tail
  // and the dropped words vanished at the page boundary ("words missing
  // between pages").  T23 above only used tiny words (<2048 total spillover)
  // so it never exercised the overflow.
  //
  // Here we feed ~800 distinct fixed-width words (~4.8 KB) in ONE onText
  // call.  Page 1 fills after a few words; the ~4.7 KB tail becomes
  // spillover — well past the old 2048 cap.  With the growing buffer every
  // word must render exactly once, in source order.
  // -----------------------------------------------------------------------
  {
    StreamingPaginatorConfig narrow = cfg;  // tiny 100x80 page
    FakeRenderer fr;
    StreamingPaginator p(narrow, fr);

    constexpr int kWordCount = 800;
    std::string text;
    text.reserve(kWordCount * 6);
    std::vector<std::string> expected;
    expected.reserve(kWordCount);
    for (int i = 0; i < kWordCount; ++i) {
      char w[8];
      std::snprintf(w, sizeof(w), "w%04d", i);  // 5 chars, all distinct
      expected.emplace_back(w);
      if (i > 0) text += ' ';
      text += w;
    }
    runner.expectTrue(text.size() > 4000, "T36_text_run_large_enough");

    // One onText delivering the whole 4.8 KB run — tail >> old 2048 cap.
    feedText(p, text);

    // Drain pages until spillover empty.  ~12 words/tiny-page → ~70 pages;
    // cap generously.
    for (int iter = 0; iter < 4000; ++iter) {
      p.resetForNextPage();
      if (p.pendingBytes() == 0 && !p.isPageFull()) break;
    }
    p.onStreamEnd();  // flush trailing line

    // Every word must appear exactly once, in source order.
    runner.expectEq(static_cast<size_t>(kWordCount), fr.drawCalls.size(),
                    "T36_all_words_drawn_no_drop");
    bool orderOk = fr.drawCalls.size() == expected.size();
    for (size_t i = 0; orderOk && i < expected.size(); ++i) {
      if (fr.drawCalls[i].text != expected[i]) orderOk = false;
    }
    runner.expectTrue(orderOk, "T36_words_in_source_order");
    // Explicitly assert the LAST word (deep in the dropped region pre-fix)
    // is present — this is the most direct check of the bug.
    bool lastPresent = false;
    for (const auto& c : fr.drawCalls) {
      if (c.text == "w0799") { lastPresent = true; break; }
    }
    runner.expectTrue(lastPresent, "T36_last_word_present");
  }

  // -----------------------------------------------------------------------
  // v3.1.0 — "красная строка": first-line paragraph indent.
  //   * First line of a paragraph starts at marginLeft + firstLineIndent.
  //   * Wrapped continuation lines start at marginLeft (no indent).
  //   * Each new paragraph (after onParagraphBreak) re-indents.
  //   * firstLineIndent == 0 → flush-left (back-compat).
  // stdConfig: 100x80, margin 5, 6 px/char, 4 px space.  marginLeft = 5.
  // -----------------------------------------------------------------------
  {
    auto cfgInd = stdConfig();
    cfgInd.firstLineIndent = 20;  // first line starts at x = 5 + 20 = 25
    FakeRenderer fr;
    StreamingPaginator p(cfgInd, fr);
    // "alpha beta" fits the first (indented) line; "gamma" wraps to line 2.
    feedText(p, "alpha beta gamma delta");
    p.onParagraphBreak();        // flush both lines, open a new paragraph
    feedText(p, "omega");        // first word of paragraph 2
    p.onStreamEnd();

    runner.expectTrue(fr.drawCalls.size() >= 5, "T37_enough_draws");
    // Paragraph 1, line 1, first word: indented.
    runner.expectEq(std::string("alpha"), fr.drawCalls[0].text, "T37_p1_first_word");
    runner.expectEq(uint16_t(25), fr.drawCalls[0].x, "T37_p1_first_line_indented");
    // Find the first word on a LOWER line (wrapped continuation) — not indented.
    const uint16_t firstY = fr.drawCalls[0].y;
    bool foundCont = false;
    for (const auto& c : fr.drawCalls) {
      if (c.y > firstY) {  // a later line within paragraph 1 (before the para break)
        if (c.text == "gamma") {
          runner.expectEq(uint16_t(5), c.x, "T37_continuation_line_flush_left");
          foundCont = true;
        }
        break;
      }
    }
    runner.expectTrue(foundCont, "T37_found_continuation_line");
    // Paragraph 2's first word is indented again.
    bool foundP2 = false;
    for (const auto& c : fr.drawCalls) {
      if (c.text == "omega") {
        runner.expectEq(uint16_t(25), c.x, "T37_p2_first_line_indented");
        foundP2 = true;
        break;
      }
    }
    runner.expectTrue(foundP2, "T37_found_p2");
  }

  // T38: firstLineIndent == 0 keeps the old flush-left layout (back-compat).
  {
    auto cfg0 = stdConfig();
    cfg0.firstLineIndent = 0;
    FakeRenderer fr;
    StreamingPaginator p(cfg0, fr);
    feedText(p, "hello world");
    p.onParagraphBreak();
    runner.expectTrue(!fr.drawCalls.empty(), "T38_has_draws");
    runner.expectEq(uint16_t(5), fr.drawCalls[0].x, "T38_no_indent_when_zero");
  }

  // T39: headings are NOT first-line-indented (centered/own alignment).
  {
    auto cfgH = stdConfig();
    cfgH.firstLineIndent = 20;
    FakeRenderer fr;
    StreamingPaginator p(cfgH, fr);
    p.onHeadingStart(2);
    feedText(p, "Title");
    p.onHeadingEnd();
    p.onStreamEnd();
    runner.expectTrue(!fr.drawCalls.empty(), "T39_has_draws");
    // Heading first word at the plain left margin (no красная-строка indent).
    runner.expectEq(uint16_t(5), fr.drawCalls[0].x, "T39_heading_not_indented");
  }

  // -----------------------------------------------------------------------
  // v3.2.0 — scene-break ornament: onThematicBreak() draws a centered
  // "* * *" on its own line (EPUB <hr>), instead of an invisible gap.
  // stdConfig: 100x80, margin 5 -> innerW 90, 6 px/char.  "* * *" = 5 chars
  // = 30 px -> centered x = 5 + (90-30)/2 = 35.
  // -----------------------------------------------------------------------
  {
    auto cfg = stdConfig();
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "before");
    p.onThematicBreak();
    feedText(p, "after");
    p.onStreamEnd();

    bool foundOrnament = false;
    for (const auto& c : fr.drawCalls) {
      if (c.text == "* * *") {
        foundOrnament = true;
        runner.expectEq(uint16_t(35), c.x, "T40_ornament_centered");
      }
    }
    runner.expectTrue(foundOrnament, "T40_ornament_drawn");
    // The surrounding text still renders.
    bool before = false, after = false;
    for (const auto& c : fr.drawCalls) {
      if (c.text == "before") before = true;
      if (c.text == "after") after = true;
    }
    runner.expectTrue(before && after, "T40_text_around_ornament_intact");
  }

  // T41: MEASURE mode (draw-suppressed) emits NO ornament draw but still
  // advances the cursor (so .idx boundaries match the DRAW pass).
  {
    auto cfg = stdConfig();
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    p.setDrawSuppressed(true);
    feedText(p, "before");
    const uint16_t yBefore = p.cursorY();
    p.onThematicBreak();
    p.onStreamEnd();
    runner.expectTrue(p.cursorY() > yBefore, "T41_cursor_advanced_in_measure");
    bool anyOrnament = false;
    for (const auto& c : fr.drawCalls)
      if (c.text == "* * *") anyOrnament = true;
    runner.expectFalse(anyOrnament, "T41_no_draw_when_suppressed");
  }

  // -----------------------------------------------------------------------
  // v3.3.0 — word hyphenation (mock dictionary breaks every char from off 3
  // to len-3, each with a visible hyphen).  stdConfig: 100x80, margin 5,
  // 6 px/char, 4 px space, lineHeight 12 -> right edge 95, innerW 90.
  // -----------------------------------------------------------------------
  // T42: an overflowing word is split — prefix(+'-') on the current line, the
  // remainder on the next, with NO characters lost.
  {
    auto cfg = stdConfig();
    cfg.hyphenate = true;
    std::strncpy(cfg.hyphenLang, "xx", sizeof(cfg.hyphenLang));  // mock ignores
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    // "aa" then a 14-char word that overflows the line and must hyphenate.
    feedText(p, "aa cccccccccccccc");
    p.onParagraphBreak();
    p.onStreamEnd();

    // Exactly one drawn token ends with '-' (the hyphenated prefix).
    int hyphenated = 0;
    size_t cCount = 0;
    bool prefixThenRemainderOnDifferentLines = false;
    uint16_t hyphenY = 0;
    for (const auto& d : fr.drawCalls) {
      for (char ch : d.text) if (ch == 'c') ++cCount;
      if (!d.text.empty() && d.text.back() == '-') { ++hyphenated; hyphenY = d.y; }
    }
    for (const auto& d : fr.drawCalls) {
      // remainder = pure 'c' run on a LOWER line than the hyphenated prefix.
      if (hyphenated && d.y > hyphenY && d.text.find('c') != std::string::npos) {
        prefixThenRemainderOnDifferentLines = true;
      }
    }
    runner.expectEq(1, hyphenated, "T42_one_hyphenated_prefix");
    runner.expectEq(size_t(14), cCount, "T42_no_chars_lost");  // 14 c's, hyphen extra
    runner.expectTrue(prefixThenRemainderOnDifferentLines, "T42_remainder_on_next_line");
  }

  // T43: intra-page safety — when only ONE line is left on the page, the word
  // must NOT be hyphenated across the page boundary (it wraps/carries whole).
  {
    auto cfg = stdConfig();
    cfg.hyphenate = true;
    std::strncpy(cfg.hyphenLang, "xx", sizeof(cfg.hyphenLang));
    // pageHeight so only ~1 content line fits: margins 5+5=10, lineHeight 12
    // -> bottom=cfg.pageHeight-5; need cursorY + 2*12 > bottom on line 1.
    cfg.pageHeight = 28;  // bottom=23; first line top=5; 5+24=29 > 23 -> gate blocks
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "aa cccccccccccccc");
    // No hyphen should appear (gate prevents cross-page split).
    bool anyHyphen = false;
    for (const auto& d : fr.drawCalls)
      if (!d.text.empty() && d.text.back() == '-') anyHyphen = true;
    runner.expectFalse(anyHyphen, "T43_no_hyphen_when_one_line_left");
  }

  // T44: hyphenate=false keeps the old whole-word wrap (no '-').
  {
    auto cfg = stdConfig();
    cfg.hyphenate = false;
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    feedText(p, "aa cccccccccccccc");
    p.onParagraphBreak();
    p.onStreamEnd();
    bool anyHyphen = false;
    for (const auto& d : fr.drawCalls)
      if (!d.text.empty() && d.text.back() == '-') anyHyphen = true;
    runner.expectFalse(anyHyphen, "T44_no_hyphen_when_disabled");
  }

  // -----------------------------------------------------------------------
  // v3.5.2 — atParagraphStart restore on resume.  A page resumed mid-sentence
  // must NOT get the красная-строка first-line indent; a paragraph-start page
  // must.  setAtParagraphStart(false) is what the render path calls for a
  // mid-paragraph resumed page.  stdConfig: marginLeft 5, indent 20 -> x = 25
  // when indented, 5 when not.
  // -----------------------------------------------------------------------
  {
    auto cfg = stdConfig();
    cfg.firstLineIndent = 20;
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);
    p.setAtParagraphStart(false);  // resumed into a mid-paragraph continuation
    feedText(p, "continuation");
    p.onParagraphBreak();
    runner.expectTrue(!fr.drawCalls.empty(), "T45_has_draws");
    runner.expectEq(uint16_t(5), fr.drawCalls[0].x, "T45_midparagraph_resume_no_indent");
  }
  {
    auto cfg = stdConfig();
    cfg.firstLineIndent = 20;
    FakeRenderer fr;
    StreamingPaginator p(cfg, fr);  // ctor -> atParagraphStart = true
    feedText(p, "paragraph");
    p.onParagraphBreak();
    runner.expectEq(uint16_t(25), fr.drawCalls[0].x, "T45_paragraph_start_indents");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
