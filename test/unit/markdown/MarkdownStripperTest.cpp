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

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
