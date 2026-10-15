#include "test_utils.h"

#include <Markers.h>
#include <MarkdownStripper.h>

#include <string>
#include <vector>

// =============================================================================
// MarkdownStripperTest — Markdown → marker-byte conversion.
//
// Verifies the token→marker mapping (headings, bold/italic, lists, blockquote,
// hr, code, images), paragraph handling (blank line = break, soft-wrap = join),
// and the two improvements over the legacy per-line reset (code-fence toggle,
// inline auto-close at paragraph boundaries).
// =============================================================================

using snapix::markdown::MarkdownStripper;
using snapix::smolport::HtmlStripperSink;
using namespace snapix::smolport;  // marker constants

namespace {

struct CollectSink : public HtmlStripperSink {
  std::vector<uint8_t> data;
  void emit(const uint8_t* d, size_t len) override { data.insert(data.end(), d, d + len); }
};

std::vector<uint8_t> run(const std::string& in) {
  CollectSink sink;
  MarkdownStripper s(sink);
  s.feed(reinterpret_cast<const uint8_t*>(in.data()), in.size());
  s.finish();
  return sink.data;
}

// Extract plain text, dropping markers (and the level digit after kHeadingOn,
// un-doubling escaped 0x01).
std::string plainText(const std::vector<uint8_t>& v) {
  std::string out;
  size_t i = 0;
  while (i < v.size()) {
    if (v[i] == kMarker && i + 1 < v.size()) {
      const uint8_t tag = v[i + 1];
      if (tag == kMarker) {
        out += static_cast<char>(kMarker);
        i += 2;
      } else if (tag == kHeadingOn) {
        i += 3;  // 0x01 'H' <digit>
      } else {
        i += 2;
      }
    } else {
      out += static_cast<char>(v[i]);
      ++i;
    }
  }
  return out;
}

bool hasMarker(const std::vector<uint8_t>& v, uint8_t tag) {
  for (size_t i = 0; i + 1 < v.size(); ++i)
    if (v[i] == kMarker && v[i + 1] == tag) return true;
  return false;
}

int countMarker(const std::vector<uint8_t>& v, uint8_t tag) {
  int n = 0;
  for (size_t i = 0; i + 1 < v.size(); ++i)
    if (v[i] == kMarker && v[i + 1] == tag) ++n;
  return n;
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("MarkdownStripper");

  // T1: plain paragraph → verbatim text, no markers.
  {
    auto v = run("Just some text.");
    runner.expectEqual("Just some text.", plainText(v), "T1_plain_text");
    runner.expectEq(0, countMarker(v, kParagraphBreak), "T1_no_breaks");
  }

  // T2: heading → kHeadingOn + level digit + kHeadingOff, text preserved.
  {
    auto v = run("# Title");
    runner.expectTrue(hasMarker(v, kHeadingOn), "T2_heading_on");
    runner.expectTrue(hasMarker(v, kHeadingOff), "T2_heading_off");
    runner.expectEqual("Title", plainText(v), "T2_heading_text");
    // level digit '1' immediately follows kHeadingOn
    bool lvlOk = false;
    for (size_t i = 0; i + 2 < v.size(); ++i)
      if (v[i] == kMarker && v[i + 1] == kHeadingOn && v[i + 2] == '1') lvlOk = true;
    runner.expectTrue(lvlOk, "T2_heading_level_1");
  }

  // T2b: h3 → level digit '3'.
  {
    auto v = run("### Deep");
    bool lvlOk = false;
    for (size_t i = 0; i + 2 < v.size(); ++i)
      if (v[i] == kMarker && v[i + 1] == kHeadingOn && v[i + 2] == '3') lvlOk = true;
    runner.expectTrue(lvlOk, "T2b_heading_level_3");
  }

  // T3: bold.
  {
    auto v = run("a **bold** word");
    runner.expectTrue(hasMarker(v, kBoldOn) && hasMarker(v, kBoldOff), "T3_bold_markers");
    runner.expectEqual("a bold word", plainText(v), "T3_bold_text");
  }

  // T4: italic.
  {
    auto v = run("an *em* word");
    runner.expectTrue(hasMarker(v, kItalicOn) && hasMarker(v, kItalicOff), "T4_italic_markers");
    runner.expectEqual("an em word", plainText(v), "T4_italic_text");
  }

  // T5: two paragraphs (blank line) → exactly one paragraph break.
  {
    auto v = run("para one\n\npara two");
    runner.expectEq(1, countMarker(v, kParagraphBreak), "T5_one_break");
    runner.expectEqual("para onepara two", plainText(v), "T5_text");
  }

  // T6: soft-wrapped lines (no blank) → joined with a space, no break.
  {
    auto v = run("line one\nline two");
    runner.expectEq(0, countMarker(v, kParagraphBreak), "T6_no_break");
    runner.expectEqual("line one line two", plainText(v), "T6_join_space");
  }

  // T7: horizontal rule → kBreak ornament.
  {
    auto v = run("above\n\n---\n\nbelow");
    runner.expectTrue(hasMarker(v, kBreak), "T7_hr_break");
  }

  // T8: blockquote.
  {
    auto v = run("> quoted line");
    runner.expectTrue(hasMarker(v, kQuoteOn) && hasMarker(v, kQuoteOff), "T8_quote_markers");
    runner.expectEqual("quoted line", plainText(v), "T8_quote_text");
  }

  // T9: unordered list item → bullet prefix.
  {
    auto v = run("- first item");
    runner.expectEqual("\xE2\x80\xA2 first item", plainText(v), "T9_bullet");
  }

  // T10: ordered list item → number prefix.
  {
    auto v = run("3. third");
    runner.expectEqual("3. third", plainText(v), "T10_ordered");
  }

  // T11: inline code → italic span, text preserved.
  {
    auto v = run("use `printf` here");
    runner.expectTrue(hasMarker(v, kItalicOn) && hasMarker(v, kItalicOff), "T11_code_italic");
    runner.expectEqual("use printf here", plainText(v), "T11_code_text");
  }

  // T12: fenced code block opens AND closes (toggle survives per-line reset).
  {
    auto v = run("```\ncode line\n```");
    runner.expectTrue(hasMarker(v, kItalicOn), "T12_code_open_italic");
    // balanced italic on/off → code block closed
    runner.expectEq(countMarker(v, kItalicOn), countMarker(v, kItalicOff), "T12_italic_balanced");
    runner.expectTrue(plainText(v).find("[Code:") != std::string::npos, "T12_code_label");
    runner.expectTrue(plainText(v).find("]") != std::string::npos, "T12_code_close_bracket");
  }

  // T13: heading immediately followed by paragraph (no blank) → break between.
  {
    auto v = run("# Head\nbody text");
    runner.expectTrue(hasMarker(v, kHeadingOff), "T13_heading_off");
    runner.expectTrue(countMarker(v, kParagraphBreak) >= 1, "T13_break_after_heading");
    runner.expectEqual("Headbody text", plainText(v), "T13_text");
  }

  // T14: chunk boundary mid-line — line buffer survives.
  {
    CollectSink sink;
    MarkdownStripper s(sink);
    s.feed(reinterpret_cast<const uint8_t*>("**bo"), 4);
    s.feed(reinterpret_cast<const uint8_t*>("ld**"), 4);
    s.finish();
    runner.expectTrue(hasMarker(sink.data, kBoldOn) && hasMarker(sink.data, kBoldOff),
                      "T14_chunk_boundary_bold");
    runner.expectEqual("bold", plainText(sink.data), "T14_chunk_boundary_text");
  }

  // T15: leading blank lines suppressed (no leading paragraph break).
  {
    auto v = run("\n\nFirst.");
    runner.expectEq(0, countMarker(v, kParagraphBreak), "T15_no_leading_break");
    runner.expectEqual("First.", plainText(v), "T15_text");
  }

  // T16: image → "[Image]" placeholder.
  {
    auto v = run("![alt](pic.png)");
    runner.expectTrue(plainText(v).find("[Image]") != std::string::npos, "T16_image_placeholder");
  }

  // T17: inline auto-close — bold opened but never closed doesn't bleed past
  // a paragraph break (a kBoldOff is emitted before the break).
  {
    auto v = run("**oops\n\nnext");
    // there must be at least as many bold-off as bold-on (auto-closed)
    runner.expectTrue(countMarker(v, kBoldOff) >= 1, "T17_bold_autoclosed");
  }

  // -- text hygiene (TextHygiene.h, same gate as HtmlStripper) --

  // H1: a BOM at the start of the file is dropped before md_parser sees the
  // line, so the heading is still a heading.
  {
    auto v = run("\xEF\xBB\xBF# Title\nbody");
    runner.expectTrue(hasMarker(v, kHeadingOn), "H1_bom_heading_detected");
    runner.expectEqual("Titlebody", plainText(v), "H1_bom_dropped");
    runner.expectEq(0, countMarker(run("\xEF\xBB\xBF\n\nFirst."), kParagraphBreak), "H1_bom_not_content");
  }

  // H2: soft hyphen / ZWSP inside words disappear, also inside styled spans.
  runner.expectEqual("\xD0\xBF\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB5\xD0\xB4",
                     plainText(run("\xD0\xBF\xD0\xBE\xC2\xAD\xD1\x81\xD0\xBB\xD0\xB5\xE2\x80\x8B\xD0\xB4")),
                     "H2_shy_zwsp_in_word");
  {
    auto v = run("**bo\xC2\xADld** it\xE2\x81\xA0" "alic");
    runner.expectTrue(hasMarker(v, kBoldOn) && hasMarker(v, kBoldOff), "H2_bold_still_bold");
    runner.expectEqual("bold italic", plainText(v), "H2_styled_text");
  }

  // H3: C1 controls are windows-1252; undefined slots dropped.
  runner.expectEqual("a\xE2\x80\x94" "b\xE2\x80\xA6" "c", plainText(run("a\xC2\x97" "b\xC2\x85" "c\xC2\x81")),
                     "H3_c1_cp1252");

  // H4: characters sharing a lead byte with a dropped one are kept intact.
  {
    const std::string keep =
        "\xC2\xA9 \xC2\xAB\xE2\x80\x94\xE2\x80\xA6\xC2\xBB \xE2\x82\xAC "
        "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x98\x80";
    runner.expectEqual(keep, plainText(run(keep)), "H4_same_lead_bytes_kept");
  }

  // H5: C0 controls dropped, VT/FF → space, U+2028 → space, 0x01 still escaped.
  {
    auto v = run(std::string("a\x00" "b\x07" "c\x0B" "d\xE2\x80\xA8" "e\x01" "f", 13));
    runner.expectEqual(std::string("abc d e\x01" "f"), plainText(v), "H5_controls");
    std::string esc(2, static_cast<char>(kMarker));
    runner.expectTrue(std::string(v.begin(), v.end()).find(esc + "f") != std::string::npos,
                      "H5_marker_escape_kept");
  }

  // H6: malformed UTF-8 passes through verbatim, including a tail at EOF.
  runner.expectEqual("a\x80" "b\xD0 c", plainText(run("a\x80" "b\xD0\nc")), "H6_malformed_verbatim");
  runner.expectEqual("x\xE2\x80", plainText(run("x\xE2\x80")), "H6_truncated_at_eof");

  // H7: every feed() split, and byte-at-a-time feeding, match one pass.
  {
    const std::string src =
        "\xEF\xBB\xBF# \xD0\x93\xD0\xBB\xD0\xB0\xC2\xAD\xD0\xB2\xD0\xB0\n\n"
        "**\xD0\xB6\xD0\xB8\xE2\x80\x8B\xD1\x80** \xC2\xA9\xC2\xAB\xE2\x80\x94\xC2\xBB \xC2\x97\n"
        "- \xE2\x82\xAC\xF0\x9F\x98\x80\xF3\xA0\x80\x81 item\n\n> q\xE2\x80\xA9uote\nx\xE2\x80";
    const auto whole = run(src);
    bool allSplitsOk = true;
    for (size_t cut = 0; cut <= src.size(); ++cut) {
      CollectSink sink;
      MarkdownStripper s(sink);
      s.feed(reinterpret_cast<const uint8_t*>(src.data()), cut);
      s.feed(reinterpret_cast<const uint8_t*>(src.data()) + cut, src.size() - cut);
      s.finish();
      if (sink.data != whole) allSplitsOk = false;
    }
    runner.expectTrue(allSplitsOk, "H7_every_split");
    CollectSink sink;
    MarkdownStripper s(sink);
    for (size_t i = 0; i < src.size(); ++i) s.feed(reinterpret_cast<const uint8_t*>(src.data()) + i, 1);
    s.finish();
    runner.expectTrue(sink.data == whole, "H7_bytewise");
    runner.expectTrue(hasMarker(whole, kHeadingOn) && hasMarker(whole, kBoldOn) && hasMarker(whole, kQuoteOn),
                      "H7_structure");
    const std::string text = plainText(whole);
    runner.expectTrue(text.find("\xEF\xBB\xBF") == std::string::npos && text.find("\xC2\xAD") == std::string::npos &&
                          text.find("\xE2\x80\x8B") == std::string::npos &&
                          text.find("\xF3\xA0\x80\x81") == std::string::npos,
                      "H7_invisibles_gone");
    runner.expectTrue(text.find("\xD0\x93\xD0\xBB\xD0\xB0\xD0\xB2\xD0\xB0") != std::string::npos &&
                          text.find("\xC2\xA9\xC2\xAB\xE2\x80\x94\xC2\xBB \xE2\x80\x94") != std::string::npos &&
                          text.find("\xE2\x82\xAC\xF0\x9F\x98\x80 item") != std::string::npos,
                      "H7_text_kept");
  }

  // H8: an over-long line is split for the line buffer without cutting a
  // multi-byte character in half.
  {
    std::string src;
    for (int i = 0; i < 300; ++i) src += "\xD0\x96";  // Ж × 300 = 600 bytes, one line
    const std::string text = plainText(run(src));
    std::string joined;
    for (const char c : text)
      if (c != ' ') joined += c;
    runner.expectEqual(src, joined, "H8_long_line_all_bytes");
    bool splitMidChar = false;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] != ' ') continue;
      if (i > 0 && static_cast<uint8_t>(text[i - 1]) == 0xD0) splitMidChar = true;
      if (i + 1 < text.size() && static_cast<uint8_t>(text[i + 1]) == 0x96 &&
          (i == 0 || static_cast<uint8_t>(text[i - 1]) != 0xD0))
        splitMidChar = true;
    }
    runner.expectTrue(!splitMidChar, "H8_long_line_no_split_char");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
