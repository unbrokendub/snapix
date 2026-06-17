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

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
