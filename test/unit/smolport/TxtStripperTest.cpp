#include "test_utils.h"

#include <Markers.h>
#include <TxtStripper.h>

#include <string>
#include <vector>

// =============================================================================
// TxtStripperTest — plain-text → marker-byte conversion.
//
// The stripper is a near-passthrough: the only transform is paragraph
// detection (run of newlines → ONE kParagraphBreak), with leading/trailing
// newline runs suppressed and chunk-boundary state preserved.
// =============================================================================

using snapix::smolport::HtmlStripperSink;
using snapix::smolport::kMarker;
using snapix::smolport::kParagraphBreak;
using snapix::smolport::TxtStripper;

namespace {

// Collects everything the stripper emits.
struct CollectSink : public HtmlStripperSink {
  std::vector<uint8_t> data;
  void emit(const uint8_t* d, size_t len) override { data.insert(data.end(), d, d + len); }
};

std::string toStr(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

std::string feedAll(const std::string& in) {
  CollectSink sink;
  TxtStripper s(sink);  // reflow = false (every newline run → paragraph)
  s.feed(reinterpret_cast<const uint8_t*>(in.data()), in.size());
  s.finish();
  return toStr(sink.data);
}

std::string feedReflow(const std::string& in) {
  CollectSink sink;
  TxtStripper s(sink, /*reflow=*/true);  // single \n → space, blank line → paragraph
  s.feed(reinterpret_cast<const uint8_t*>(in.data()), in.size());
  s.finish();
  return toStr(sink.data);
}

// A paragraph-break marker as a 2-char string.
const std::string PB = std::string(1, static_cast<char>(kMarker)) + static_cast<char>(kParagraphBreak);

// Markerize a set of paragraph-aligned chunks INDEPENDENTLY (fresh stripper per
// chunk; startPendingBreak = index>0) and concatenate — mirrors chunked (lazy)
// markerize.  Must equal one continuous pass over the joined source.
std::string feedChunkedReflow(const std::vector<std::string>& chunks) {
  std::string out;
  for (size_t i = 0; i < chunks.size(); ++i) {
    CollectSink sink;
    TxtStripper s(sink, /*reflow=*/true, /*startPendingBreak=*/i > 0);
    s.feed(reinterpret_cast<const uint8_t*>(chunks[i].data()), chunks[i].size());
    s.finish();
    out += toStr(sink.data);
  }
  return out;
}

// Same, non-reflow mode (every newline is a paragraph break).
std::string feedChunkedNonReflow(const std::vector<std::string>& chunks) {
  std::string out;
  for (size_t i = 0; i < chunks.size(); ++i) {
    CollectSink sink;
    TxtStripper s(sink, /*reflow=*/false, /*startPendingBreak=*/i > 0);
    s.feed(reinterpret_cast<const uint8_t*>(chunks[i].data()), chunks[i].size());
    s.finish();
    out += toStr(sink.data);
  }
  return out;
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("TxtStripper");

  // T1: empty input → no output.
  runner.expectEqual("", feedAll(""), "T1_empty");

  // T2: plain text → verbatim passthrough, no markers.
  runner.expectEqual("Hello, world.", feedAll("Hello, world."), "T2_passthrough");

  // T3: single newline → one paragraph break (every line = paragraph, legacy model).
  runner.expectEqual("para1" + PB + "para2", feedAll("para1\npara2"), "T3_single_newline_break");

  // T4: run of newlines collapses to ONE break.
  runner.expectEqual("a" + PB + "b", feedAll("a\n\n\nb"), "T4_collapse_blank_lines");

  // T5: leading newlines suppressed (no break before any content).
  runner.expectEqual("Hello", feedAll("\n\nHello"), "T5_leading_suppressed");

  // T6: trailing newlines emit no trailing break.
  runner.expectEqual("Hello", feedAll("Hello\n\n"), "T6_trailing_suppressed");

  // T7: CRLF — the CR is ignored, the LF drives a single break.
  runner.expectEqual("a" + PB + "b", feedAll("a\r\nb"), "T7_crlf");

  // T8: literal 0x01 in text is self-doubled (marker escape).
  {
    std::string in(1, 'x');
    in += static_cast<char>(0x01);
    in += 'y';
    std::string expect(1, 'x');
    expect += static_cast<char>(kMarker);
    expect += static_cast<char>(kMarker);
    expect += 'y';
    runner.expectEqual(expect, feedAll(in), "T8_escape_0x01");
  }

  // T9: tab passes through as text (StreamingPaginator treats it as a separator).
  runner.expectEqual("a\tb", feedAll("a\tb"), "T9_tab_passthrough");

  // T10: chunk boundary between content and newline → one break, state preserved.
  {
    CollectSink sink;
    TxtStripper s(sink);
    s.feed(reinterpret_cast<const uint8_t*>("para1\n"), 6);
    s.feed(reinterpret_cast<const uint8_t*>("para2"), 5);
    s.finish();
    runner.expectEqual("para1" + PB + "para2", toStr(sink.data), "T10_chunk_boundary");
  }

  // T11: newline run split across a chunk boundary still collapses to one break.
  {
    CollectSink sink;
    TxtStripper s(sink);
    s.feed(reinterpret_cast<const uint8_t*>("a\n"), 2);
    s.feed(reinterpret_cast<const uint8_t*>("\nb"), 2);
    s.finish();
    runner.expectEqual("a" + PB + "b", toStr(sink.data), "T11_break_across_chunks");
  }

  // T12: multi-line paragraphs each separated by exactly one break (3 lines → 2 breaks).
  runner.expectEqual("l1" + PB + "l2" + PB + "l3", feedAll("l1\nl2\nl3"), "T12_three_lines");

  // T13: reset() clears paragraph state for a fresh document.
  {
    CollectSink sink;
    TxtStripper s(sink);
    s.feed(reinterpret_cast<const uint8_t*>("first"), 5);
    s.reset();
    sink.data.clear();
    s.feed(reinterpret_cast<const uint8_t*>("\nsecond"), 7);  // leading \n must be suppressed post-reset
    s.finish();
    runner.expectEqual("second", toStr(sink.data), "T13_reset");
  }

  // -- reflow mode (hard-wrapped text) --

  // R1: single newline → space (lines join into one paragraph).
  runner.expectEqual("line one line two", feedReflow("line one\nline two"), "R1_softwrap_join");

  // R2: blank line → paragraph break.
  runner.expectEqual("para one" + PB + "para two", feedReflow("para one\n\npara two"), "R2_blank_break");

  // R3: hard-wrapped paragraph (several short lines) reflows, blank line breaks.
  runner.expectEqual("a b c" + PB + "d e", feedReflow("a\nb\nc\n\nd\ne"), "R3_hardwrap");

  // R4: 3+ newlines still collapse to ONE paragraph break.
  runner.expectEqual("x" + PB + "y", feedReflow("x\n\n\n\ny"), "R4_collapse");

  // R5: leading newlines suppressed.
  runner.expectEqual("first", feedReflow("\n\nfirst"), "R5_leading");

  // R6: chunk boundary mid-softwrap → still a single space.
  {
    CollectSink sink;
    TxtStripper s(sink, /*reflow=*/true);
    s.feed(reinterpret_cast<const uint8_t*>("aaa\n"), 4);
    s.feed(reinterpret_cast<const uint8_t*>("bbb"), 3);
    s.finish();
    runner.expectEqual("aaa bbb", toStr(sink.data), "R6_softwrap_across_chunks");
  }

  // -- chunked markerize equivalence (lazy markerize correctness) --

  // R7: paragraph-aligned chunks concatenate identically to one continuous pass.
  {
    const std::string src = "para one\n\npara two\n\npara three";
    // Split AT paragraph boundaries (each chunk includes its trailing blank line).
    const std::vector<std::string> chunks = {"para one\n\n", "para two\n\n", "para three"};
    runner.expectEqual(feedReflow(src), feedChunkedReflow(chunks), "R7_chunk_equiv_3");
  }

  // R8: two-chunk split, content preserved + exactly the right breaks.
  {
    const std::string src = "alpha beta\n\ngamma delta";
    const std::vector<std::string> chunks = {"alpha beta\n\n", "gamma delta"};
    runner.expectEqual(feedReflow(src), feedChunkedReflow(chunks), "R8_chunk_equiv_2");
    // sanity: the joined result is exactly "alpha beta" PB "gamma delta"
    runner.expectEqual("alpha beta" + PB + "gamma delta", feedChunkedReflow(chunks), "R8_exact");
  }

  // R9: split where the boundary blank line straddles the cut still works
  // (chunk 0 ends with one '\n', chunk 1 begins with the second '\n').
  {
    const std::string src = "one two\n\nthree four";
    const std::vector<std::string> chunks = {"one two\n", "\nthree four"};
    runner.expectEqual(feedReflow(src), feedChunkedReflow(chunks), "R9_straddle");
  }

  // R10: startPendingBreak alone emits a leading break before the first text.
  {
    CollectSink sink;
    TxtStripper s(sink, /*reflow=*/true, /*startPendingBreak=*/true);
    s.feed(reinterpret_cast<const uint8_t*>("hello"), 5);
    s.finish();
    runner.expectEqual(PB + "hello", toStr(sink.data), "R10_forced_break");
  }

  // -- chunked markerize equivalence, NON-reflow (every newline = paragraph) --
  // Mirrors the lazy markerize cut: chunks split AFTER a '\n' (the boundary
  // findChunkEnd uses in non-reflow mode).

  // R11: line-per-paragraph chunks concatenate identically to one pass.
  {
    const std::string src = "alpha\nbeta\ngamma";
    const std::vector<std::string> chunks = {"alpha\n", "beta\n", "gamma"};
    runner.expectEqual(feedAll(src), feedChunkedNonReflow(chunks), "R11_nonreflow_equiv");
    runner.expectEqual("alpha" + PB + "beta" + PB + "gamma", feedChunkedNonReflow(chunks),
                       "R11_exact");
  }

  // R12: a newline RUN split across the cut still collapses to ONE break
  // (chunk 0 ends after the first '\n'; chunk 1 carries the rest of the run).
  {
    const std::string src = "alpha\n\n\nbeta";
    const std::vector<std::string> chunks = {"alpha\n", "\n\nbeta"};
    runner.expectEqual(feedAll(src), feedChunkedNonReflow(chunks), "R12_nonreflow_run_split");
    runner.expectEqual("alpha" + PB + "beta", feedChunkedNonReflow(chunks), "R12_exact");
  }

  // R13: three chunks, mixed run lengths — all collapse, no spurious breaks.
  {
    const std::string src = "one\ntwo\n\nthree";
    const std::vector<std::string> chunks = {"one\n", "two\n", "\nthree"};
    runner.expectEqual(feedAll(src), feedChunkedNonReflow(chunks), "R13_nonreflow_mixed");
  }

  // -- text hygiene (TextHygiene.h, same gate as HtmlStripper) --

  // H1: a UTF-8 BOM at the start of the file is dropped.
  runner.expectEqual("Hello", feedAll("\xEF\xBB\xBFHello"), "H1_bom_at_start");

  // H2: a BOM is not content — the blank lines after it stay suppressed.
  runner.expectEqual("Hello", feedAll("\xEF\xBB\xBF\n\nHello"), "H2_bom_then_blank_lines");
  runner.expectEqual("Hello", feedReflow("\xEF\xBB\xBF\n\nHello"), "H2_bom_then_blank_lines_reflow");

  // H3: soft hyphen / ZWSP / word joiner / LRM inside words disappear.
  runner.expectEqual("\xD0\xBF\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB5\xD0\xB4",
                     feedAll("\xD0\xBF\xD0\xBE\xC2\xAD\xD1\x81\xD0\xBB\xD0\xB5\xD0\xB4"),
                     "H3_soft_hyphen_in_word");
  runner.expectEqual("foobar", feedAll("foo\xE2\x80\x8B" "bar"), "H3_zwsp_in_word");
  runner.expectEqual("foobar", feedAll("foo\xE2\x81\xA0" "bar"), "H3_word_joiner");
  runner.expectEqual("ab", feedAll("a\xE2\x80\x8E" "b"), "H3_lrm");
  runner.expectEqual("ab", feedAll("a\xEF\xB8\x8F" "b"), "H3_variation_selector");
  runner.expectEqual("ab", feedAll("a\xF3\xA0\x80\x81" "b"), "H3_tag_character");

  // H4: C1 controls are windows-1252 (U+0097 → em dash, U+0085 → ellipsis);
  // undefined slots (U+0081) are dropped.
  runner.expectEqual("a\xE2\x80\x94" "b\xE2\x80\xA6", feedAll("a\xC2\x97" "b\xC2\x85"), "H4_c1_cp1252");
  runner.expectEqual("ab", feedAll("a\xC2\x81" "b"), "H4_c1_undefined_dropped");

  // H5: characters that share a lead byte with a dropped one are kept intact:
  // © « — … » € NBSP, Cyrillic, a 4-byte emoji.
  {
    const std::string keep =
        "\xC2\xA9 \xC2\xAB\xE2\x80\x94\xE2\x80\xA6\xC2\xBB \xE2\x82\xAC\xC2\xA0"
        "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x98\x80";
    runner.expectEqual(keep, feedAll(keep), "H5_same_lead_bytes_kept");
  }

  // H6: C0 controls and DEL dropped; VT / FF become a space; tab kept.
  runner.expectEqual("abc", feedAll(std::string("a\x00" "b\x07" "c\x7F", 6)), "H6_c0_dropped");
  runner.expectEqual("a b c\td", feedAll("a\x0B" "b\x0C" "c\td"), "H6_vt_ff_space");

  // H7: LINE / PARAGRAPH SEPARATOR → space.
  runner.expectEqual("a b c", feedAll("a\xE2\x80\xA8" "b\xE2\x80\xA9" "c"), "H7_line_separator");

  // H8: malformed UTF-8 passes through verbatim — truncated before a newline,
  // a lone continuation byte, an invalid lead, a truncated tail at EOF.
  runner.expectEqual("a\xD0" + PB + "b", feedAll("a\xD0\nb"), "H8_truncated_before_newline");
  runner.expectEqual("a\x80" "b\xFF" "c\xC0", feedAll("a\x80" "b\xFF" "c\xC0"), "H8_invalid_bytes");
  runner.expectEqual("x\xE2\x80", feedAll("x\xE2\x80"), "H8_truncated_at_eof");
  runner.expectEqual("x\xE2" "yz", feedAll("x\xE2" "yz"), "H8_truncated_by_ascii");

  // H9: 0x01 escape still doubled next to a dropped character.
  {
    std::string in = "\x01\xC2\xAD" "x";
    std::string expect = std::string(2, static_cast<char>(kMarker)) + "x";
    runner.expectEqual(expect, feedAll(in), "H9_escape_next_to_dropped");
  }

  // H10: a dropped character between newlines does not split the run —
  // a line holding only a soft hyphen is blank.
  runner.expectEqual("a" + PB + "b", feedAll("a\n\xC2\xAD\nb"), "H10_invisible_only_line_nonreflow");
  runner.expectEqual("a" + PB + "b", feedReflow("a\n\xE2\x80\x8B\nb"), "H10_invisible_only_line_reflow");

  // H11: every UTF-8 sequence split at every feed() boundary — output equals
  // one pass (the filter holds the partial sequence across calls).
  {
    const std::string src =
        "\xEF\xBB\xBF\xD0\x9F\xD0\xBE\xC2\xAD\xD1\x81\xD0\xBB\xD0\xB5 \xC2\xA9\xC2\xAB\xE2\x80\x94"
        "\xE2\x80\x8B\xC2\x97\xE2\x80\xA6\xC2\xBB\n\n\xE2\x82\xAC\xF0\x9F\x98\x80\xF3\xA0\x80\x81"
        "\xE2\x80\xA8z\xD0\nq\xE2\x80";
    for (int reflow = 0; reflow < 2; ++reflow) {
      const std::string whole = reflow ? feedReflow(src) : feedAll(src);
      bool allSplitsOk = true;
      for (size_t cut = 0; cut <= src.size(); ++cut) {
        CollectSink sink;
        TxtStripper s(sink, reflow != 0);
        s.feed(reinterpret_cast<const uint8_t*>(src.data()), cut);
        s.feed(reinterpret_cast<const uint8_t*>(src.data()) + cut, src.size() - cut);
        s.finish();
        if (toStr(sink.data) != whole) allSplitsOk = false;
      }
      CollectSink sink;
      TxtStripper s(sink, reflow != 0);
      for (size_t i = 0; i < src.size(); ++i) s.feed(reinterpret_cast<const uint8_t*>(src.data()) + i, 1);
      s.finish();
      runner.expectTrue(allSplitsOk, reflow ? "H11_every_split_reflow" : "H11_every_split");
      runner.expectEqual(whole, toStr(sink.data), reflow ? "H11_bytewise_reflow" : "H11_bytewise");
    }
    // And the one-pass result itself is right (BOM, SHY, ZWSP, tag char gone,
    // U+0097 → —, U+2028 → space, truncated bytes verbatim).
    runner.expectEqual(
        "\xD0\x9F\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB5 \xC2\xA9\xC2\xAB\xE2\x80\x94\xE2\x80\x94\xE2\x80\xA6\xC2\xBB" +
            PB + "\xE2\x82\xAC\xF0\x9F\x98\x80 z\xD0" + PB + "q\xE2\x80",
        feedAll(src), "H11_one_pass_exact");
  }

  // H12: chunked (lazy) markerize equivalence with dropped characters at the
  // chunk edges — paragraph-aligned chunks still concatenate byte-identically.
  {
    const std::string src = "one\xC2\xAD two\n\n\xEF\xBB\xBFthree\xE2\x80\x8B four\n\n\xC2\x97 five";
    const std::vector<std::string> chunks = {"one\xC2\xAD two\n\n", "\xEF\xBB\xBFthree\xE2\x80\x8B four\n\n",
                                             "\xC2\x97 five"};
    runner.expectEqual(feedReflow(src), feedChunkedReflow(chunks), "H12_chunk_equiv_reflow");
    runner.expectEqual("one two" + PB + "three four" + PB + "\xE2\x80\x94 five", feedChunkedReflow(chunks),
                       "H12_exact");
  }
  {
    // A chunk holding nothing but a dropped character: emits nothing, and
    // the next chunk still gets exactly one break.
    const std::string src = "alpha\n\xC2\xAD\nbeta";
    const std::vector<std::string> chunks = {"alpha\n", "\xC2\xAD\n", "beta"};
    runner.expectEqual(feedAll(src), feedChunkedNonReflow(chunks), "H12_invisible_chunk_nonreflow");
    runner.expectEqual("alpha" + PB + "beta", feedChunkedNonReflow(chunks), "H12_invisible_chunk_exact");
  }

  // H13: reset() discards a partial sequence held by the filter.
  {
    CollectSink sink;
    TxtStripper s(sink);
    s.feed(reinterpret_cast<const uint8_t*>("\xE2\x80"), 2);
    s.reset();
    s.feed(reinterpret_cast<const uint8_t*>("x"), 1);
    s.finish();
    runner.expectEqual("x", toStr(sink.data), "H13_reset_drops_partial");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
