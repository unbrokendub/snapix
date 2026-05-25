#include "test_utils.h"

#include <HtmlStripper.h>
#include <MarkerizeChapter.h>
#include <Markers.h>

#include <cstring>
#include <string>
#include <vector>

// =============================================================================
// MarkerizeChapterTest — proves the v3 markerizer side-channel can stream
// HTML / FB2 bytes through HtmlStripper and emit a marker byte stream
// without heap allocation in the helper itself.
//
// I/O is mocked via in-memory vectors; the helper is platform-agnostic
// so the same code path runs in firmware (with LittleFS) and host tests
// (with std::vector).
// =============================================================================

using snapix::smolport::HtmlStripper;
using snapix::smolport::markerizeChapter;
using snapix::smolport::MarkerizeAbortFn;
using snapix::smolport::MarkerizeStats;
using snapix::smolport::MarkerizeStatus;

// In-memory read source — like a file but fed from a std::vector.
struct MemReader {
  std::vector<uint8_t> data;
  size_t pos = 0;
  // Optional artificial cap: max bytes returned per call (simulates
  // chunked reads from LittleFS).  0 = no cap.
  size_t perCallCap = 0;

  int read(uint8_t* buf, size_t bufSize) {
    if (pos >= data.size()) return 0;  // EOF
    size_t toCopy = std::min(bufSize, data.size() - pos);
    if (perCallCap > 0) toCopy = std::min(toCopy, perCallCap);
    std::memcpy(buf, data.data() + pos, toCopy);
    pos += toCopy;
    return static_cast<int>(toCopy);
  }
};

// In-memory write sink.
struct MemWriter {
  std::vector<uint8_t> data;
  bool failNextWrite = false;

  bool write(const uint8_t* d, size_t len) {
    if (failNextWrite) {
      failNextWrite = false;
      return false;
    }
    data.insert(data.end(), d, d + len);
    return true;
  }
};

// Helper: convert string literal → vector<uint8_t>.
static std::vector<uint8_t> bytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

// Helper: find the first occurrence of `[0x01, tag]` in the marker
// stream.  Returns SIZE_MAX if not found.
static size_t findMarker(const std::vector<uint8_t>& stream, uint8_t tag) {
  for (size_t i = 0; i + 1 < stream.size(); ++i) {
    if (stream[i] == snapix::smolport::kMarker && stream[i + 1] == tag) {
      return i;
    }
  }
  return SIZE_MAX;
}

int main() {
  TestUtils::TestRunner runner("MarkerizeChapter");

  uint8_t chunkBuf[256];

  // -----------------------------------------------------------------------
  // T1: empty input → success, zero output, EOF-terminated cleanly.
  // -----------------------------------------------------------------------
  {
    MemReader r;
    MemWriter w;
    MarkerizeStats stats{};
    auto status = markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf), {}, &stats);
    runner.expectTrue(status == MarkerizeStatus::Success, "T1_empty_success");
    runner.expectEq(uint32_t(0), stats.inputBytes, "T1_empty_input_bytes");
    runner.expectEq(uint32_t(0), stats.outputBytes, "T1_empty_output_bytes");
    runner.expectEq(size_t(0), w.data.size(), "T1_empty_no_output");
  }

  // -----------------------------------------------------------------------
  // T2: plain text passes through (no markers added).
  // -----------------------------------------------------------------------
  {
    MemReader r{bytes("Hello, world.")};
    MemWriter w;
    auto status = markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    runner.expectTrue(status == MarkerizeStatus::Success, "T2_text_success");
    runner.expectEq(std::string("Hello, world."),
                    std::string(w.data.begin(), w.data.end()), "T2_text_passthrough");
  }

  // -----------------------------------------------------------------------
  // T3: <b>text</b> emits kBoldOn ... kBoldOff markers around the text.
  // -----------------------------------------------------------------------
  {
    MemReader r{bytes("a <b>bold</b> word")};
    MemWriter w;
    auto status = markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    runner.expectTrue(status == MarkerizeStatus::Success, "T3_bold_success");
    runner.expectNe(SIZE_MAX, findMarker(w.data, snapix::smolport::kBoldOn), "T3_bold_on_present");
    runner.expectNe(SIZE_MAX, findMarker(w.data, snapix::smolport::kBoldOff), "T3_bold_off_present");
    runner.expectTrue(findMarker(w.data, snapix::smolport::kBoldOn) <
                          findMarker(w.data, snapix::smolport::kBoldOff),
                      "T3_bold_on_before_off");
  }

  // -----------------------------------------------------------------------
  // T4: chunked reads (per-call cap 7 bytes) — state survives chunk
  //     boundaries, output identical to single-call.
  // -----------------------------------------------------------------------
  {
    const std::string html = "<i>italic</i> normal <b>bold</b>";

    MemReader r1{bytes(html)};
    MemWriter w1;
    markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r1.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w1.write(d, n); },
        chunkBuf, sizeof(chunkBuf));

    MemReader r2{bytes(html)};
    r2.perCallCap = 7;  // tiny chunks
    MemWriter w2;
    markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r2.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w2.write(d, n); },
        chunkBuf, sizeof(chunkBuf));

    runner.expectEq(w1.data.size(), w2.data.size(), "T4_chunked_same_size");
    runner.expectTrue(w1.data == w2.data, "T4_chunked_identical_output");
  }

  // -----------------------------------------------------------------------
  // T5: shouldAbort fires mid-stream → returns Aborted.
  // -----------------------------------------------------------------------
  {
    MemReader r{bytes("<p>This is a longer paragraph that takes multiple chunks.</p>")};
    r.perCallCap = 4;  // force many small reads so abort can fire
    MemWriter w;
    int callCount = 0;
    auto abortFn = [&]() -> bool {
      ++callCount;
      return callCount >= 3;  // abort on third chunk boundary
    };
    auto status = markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf), abortFn);
    runner.expectTrue(status == MarkerizeStatus::Aborted, "T5_abort_status");
    runner.expectTrue(w.data.size() < r.data.size(),
                      "T5_abort_partial_output");
  }

  // -----------------------------------------------------------------------
  // T6: write error mid-stream → returns WriteError.
  // -----------------------------------------------------------------------
  {
    MemReader r{bytes("plain text content")};
    MemWriter w;
    w.failNextWrite = true;  // first write call fails
    auto status = markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    runner.expectTrue(status == MarkerizeStatus::WriteError, "T6_write_error_status");
  }

  // -----------------------------------------------------------------------
  // T7: read error mid-stream → returns ReadError.
  // -----------------------------------------------------------------------
  {
    MemWriter w;
    int reads = 0;
    auto readFn = [&](uint8_t* b, size_t n) -> int {
      ++reads;
      if (reads == 1) {
        const char* sample = "starting bytes";
        size_t len = strlen(sample);
        if (len > n) len = n;
        std::memcpy(b, sample, len);
        return static_cast<int>(len);
      }
      return -1;  // simulate I/O error on second read
    };
    auto status = markerizeChapter(
        HtmlStripper::Mode::Html, readFn,
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    runner.expectTrue(status == MarkerizeStatus::ReadError, "T7_read_error_status");
  }

  // -----------------------------------------------------------------------
  // T8: misconfigured caller (null callbacks / zero buf) → ReadError.
  // -----------------------------------------------------------------------
  {
    MemWriter w;
    auto status = markerizeChapter(
        HtmlStripper::Mode::Html, {},
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    runner.expectTrue(status == MarkerizeStatus::ReadError, "T8_null_read_returns_setup_error");
  }

  // -----------------------------------------------------------------------
  // T9: Fb2 mode dispatches differently (<emphasis> instead of <em>).
  // -----------------------------------------------------------------------
  {
    MemReader r{bytes("normal <emphasis>italic</emphasis> normal")};
    MemWriter w;
    auto status = markerizeChapter(
        HtmlStripper::Mode::Fb2,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    runner.expectTrue(status == MarkerizeStatus::Success, "T9_fb2_success");
    runner.expectNe(SIZE_MAX, findMarker(w.data, snapix::smolport::kItalicOn),
                    "T9_fb2_emphasis_to_italic");
  }

  // -----------------------------------------------------------------------
  // T10: stats correctly populated.
  // -----------------------------------------------------------------------
  {
    const std::string html = "<p>Some text <b>here</b>.</p>";
    MemReader r{bytes(html)};
    r.perCallCap = 5;
    MemWriter w;
    MarkerizeStats stats{};
    auto status = markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf), {}, &stats);
    runner.expectTrue(status == MarkerizeStatus::Success, "T10_stats_success");
    runner.expectEq(uint32_t(html.size()), stats.inputBytes, "T10_input_bytes_match");
    runner.expectEq(uint32_t(w.data.size()), stats.outputBytes, "T10_output_bytes_match");
    runner.expectTrue(stats.chunksProcessed > 1, "T10_multiple_chunks");
  }

  // -----------------------------------------------------------------------
  // v2.0.143: additional HTML tag handling — <ul>/<ol>/<li>, <div>,
  // <center>.  Lists especially are common in non-fiction EPUBs and
  // pre-fix rendered as run-on text with no visual separation.
  // -----------------------------------------------------------------------

  // T11: <ul><li>a</li><li>b</li></ul> → paragraph breaks around list,
  //   line break + bullet at each item.
  {
    MemReader r{bytes("<ul><li>alpha</li><li>beta</li></ul>")};
    MemWriter w;
    auto status = markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    runner.expectTrue(status == MarkerizeStatus::Success, "T11_list_success");
    runner.expectNe(SIZE_MAX,
                    findMarker(w.data, snapix::smolport::kParagraphBreak),
                    "T11_list_emits_paragraph_break");
    runner.expectNe(SIZE_MAX,
                    findMarker(w.data, snapix::smolport::kLineBreak),
                    "T11_list_item_line_break");
    // Bullet glyph (U+2022 = E2 80 A2) should be present in output.
    bool foundBullet = false;
    for (size_t i = 0; i + 2 < w.data.size(); ++i) {
      if (w.data[i] == 0xE2 && w.data[i + 1] == 0x80 && w.data[i + 2] == 0xA2) {
        foundBullet = true;
        break;
      }
    }
    runner.expectTrue(foundBullet, "T11_list_item_bullet_glyph");
  }

  // T12: <div>text</div> → paragraph break on close.
  {
    MemReader r{bytes("<div>some text</div>")};
    MemWriter w;
    markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    runner.expectNe(SIZE_MAX,
                    findMarker(w.data, snapix::smolport::kParagraphBreak),
                    "T12_div_close_paragraph_break");
  }

  // T13: <center>text</center> → paragraph break.
  {
    MemReader r{bytes("<center>title</center>")};
    MemWriter w;
    markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    runner.expectNe(SIZE_MAX,
                    findMarker(w.data, snapix::smolport::kParagraphBreak),
                    "T13_center_paragraph_break");
  }

  // -----------------------------------------------------------------------
  // v2.0.143: FB2 tag additions — <poem>/<stanza>, <epigraph>,
  // <text-author>.
  // -----------------------------------------------------------------------

  // T14: <epigraph>quote text</epigraph> → kQuoteOn/kQuoteOff pair.
  {
    MemReader r{bytes("<epigraph><p>wise words</p></epigraph>")};
    MemWriter w;
    markerizeChapter(
        HtmlStripper::Mode::Fb2,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    const size_t qOn = findMarker(w.data, snapix::smolport::kQuoteOn);
    const size_t qOff = findMarker(w.data, snapix::smolport::kQuoteOff);
    runner.expectNe(SIZE_MAX, qOn, "T14_epigraph_quote_on");
    runner.expectNe(SIZE_MAX, qOff, "T14_epigraph_quote_off");
    runner.expectTrue(qOn < qOff, "T14_epigraph_on_before_off");
  }

  // T15: <text-author>Author</text-author> → italic around the author
  //   name, with paragraph breaks on either side.
  {
    MemReader r{bytes("<text-author>Author Name</text-author>")};
    MemWriter w;
    markerizeChapter(
        HtmlStripper::Mode::Fb2,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    const size_t iOn = findMarker(w.data, snapix::smolport::kItalicOn);
    const size_t iOff = findMarker(w.data, snapix::smolport::kItalicOff);
    runner.expectNe(SIZE_MAX, iOn, "T15_textauthor_italic_on");
    runner.expectNe(SIZE_MAX, iOff, "T15_textauthor_italic_off");
    runner.expectTrue(iOn < iOff, "T15_textauthor_on_before_off");
    runner.expectNe(SIZE_MAX,
                    findMarker(w.data, snapix::smolport::kParagraphBreak),
                    "T15_textauthor_paragraph_break");
  }

  // T16: <poem><stanza><v>line</v></stanza></poem> → paragraph break
  //   for poem container + stanza separator, line break for verse.
  {
    MemReader r{bytes("<poem><stanza><v>Roses are red</v></stanza></poem>")};
    MemWriter w;
    markerizeChapter(
        HtmlStripper::Mode::Fb2,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    runner.expectNe(SIZE_MAX,
                    findMarker(w.data, snapix::smolport::kParagraphBreak),
                    "T16_poem_paragraph_break");
    runner.expectNe(SIZE_MAX,
                    findMarker(w.data, snapix::smolport::kLineBreak),
                    "T16_verse_line_break");
  }

  // -----------------------------------------------------------------------
  // v2.0.145 — Indent + Center marker emission.  Lists now also emit
  // kIndentOn/Off (in addition to bullets); <center>/<title> now emit
  // kCenterOn/Off; <blockquote> now emits kIndentOn/Off alongside
  // kQuote.
  // -----------------------------------------------------------------------

  // T17: <ul><li>x</li></ul> emits kIndentOn before items, kIndentOff
  //   after.  Bullets and line break still present.
  {
    MemReader r{bytes("<ul><li>alpha</li></ul>")};
    MemWriter w;
    markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    const size_t indentOn = findMarker(w.data, snapix::smolport::kIndentOn);
    const size_t indentOff = findMarker(w.data, snapix::smolport::kIndentOff);
    runner.expectNe(SIZE_MAX, indentOn, "T17_list_indent_on");
    runner.expectNe(SIZE_MAX, indentOff, "T17_list_indent_off");
    runner.expectTrue(indentOn < indentOff, "T17_list_indent_pair_order");
  }

  // T18: <center>text</center> emits kCenterOn/Off pair.
  {
    MemReader r{bytes("<center>title</center>")};
    MemWriter w;
    markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    const size_t cOn = findMarker(w.data, snapix::smolport::kCenterOn);
    const size_t cOff = findMarker(w.data, snapix::smolport::kCenterOff);
    runner.expectNe(SIZE_MAX, cOn, "T18_center_on");
    runner.expectNe(SIZE_MAX, cOff, "T18_center_off");
    runner.expectTrue(cOn < cOff, "T18_center_pair_order");
  }

  // T19: <blockquote>text</blockquote> emits both kIndent and kQuote.
  {
    MemReader r{bytes("<blockquote>quote</blockquote>")};
    MemWriter w;
    markerizeChapter(
        HtmlStripper::Mode::Html,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    runner.expectNe(SIZE_MAX,
                    findMarker(w.data, snapix::smolport::kIndentOn),
                    "T19_blockquote_indent_on");
    runner.expectNe(SIZE_MAX,
                    findMarker(w.data, snapix::smolport::kQuoteOn),
                    "T19_blockquote_quote_on");
  }

  // T20: FB2 <title>...</title> emits kCenterOn around the heading
  //   so chapter titles centre.
  {
    MemReader r{bytes("<title><p>Chapter 1</p></title>")};
    MemWriter w;
    markerizeChapter(
        HtmlStripper::Mode::Fb2,
        [&](uint8_t* b, size_t n) { return r.read(b, n); },
        [&](const uint8_t* d, size_t n) { return w.write(d, n); },
        chunkBuf, sizeof(chunkBuf));
    runner.expectNe(SIZE_MAX,
                    findMarker(w.data, snapix::smolport::kCenterOn),
                    "T20_fb2_title_center_on");
    runner.expectNe(SIZE_MAX,
                    findMarker(w.data, snapix::smolport::kCenterOff),
                    "T20_fb2_title_center_off");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
