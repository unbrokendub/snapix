#include "test_utils.h"

#include <HtmlStripper.h>
#include <MarkerizeChapter.h>
#include <Markers.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// =============================================================================
// HtmlStripperIntegrityTest — the stripper must neither DROP source text nor
// INVENT text.  Every text case is fed in one piece, one byte at a time and in
// 3-byte reads (entities / UTF-8 sequences / tags split across reads).
// =============================================================================

using snapix::smolport::HtmlStripper;
using snapix::smolport::markerizeChapter;

namespace {

struct Parsed {
  std::string text;                  // text bytes; block/line breaks → ' '
  std::vector<std::string> images;   // kImageRef payloads, in order
};

std::vector<uint8_t> markerize(const std::string& src, HtmlStripper::Mode mode, size_t chunk) {
  std::vector<uint8_t> out;
  size_t pos = 0;
  uint8_t buf[256];
  markerizeChapter(
      mode,
      [&](uint8_t* b, size_t n) -> int {
        if (pos >= src.size()) return 0;
        size_t k = std::min(n, src.size() - pos);
        if (chunk > 0) k = std::min(k, chunk);
        std::memcpy(b, src.data() + pos, k);
        pos += k;
        return static_cast<int>(k);
      },
      [&](const uint8_t* d, size_t n) -> bool {
        out.insert(out.end(), d, d + n);
        return true;
      },
      buf, sizeof(buf));
  return out;
}

Parsed parse(const std::vector<uint8_t>& m) {
  Parsed p;
  for (size_t i = 0; i < m.size(); ++i) {
    if (m[i] != snapix::smolport::kMarker) {
      p.text.push_back(static_cast<char>(m[i]));
      continue;
    }
    if (i + 1 >= m.size()) break;
    const uint8_t tag = m[i + 1];
    if (tag == snapix::smolport::kMarker) {  // escaped literal 0x01
      p.text.push_back('\x01');
      ++i;
    } else if (tag == snapix::smolport::kImageRef || tag == snapix::smolport::kAnchor) {
      if (i + 3 >= m.size()) break;
      const size_t len = m[i + 2] | (static_cast<size_t>(m[i + 3]) << 8);
      const std::string payload(reinterpret_cast<const char*>(m.data() + i + 4),
                                std::min(len, m.size() - (i + 4)));
      if (tag == snapix::smolport::kImageRef) p.images.push_back(payload);
      i += 3 + len;
    } else if (tag == snapix::smolport::kHeadingOn) {
      i += 2;
    } else {
      if (tag == snapix::smolport::kParagraphBreak || tag == snapix::smolport::kLineBreak ||
          tag == snapix::smolport::kPageBreak || tag == snapix::smolport::kBreak) {
        p.text.push_back(' ');
      }
      ++i;
    }
  }
  return p;
}

// Collapse runs of ASCII whitespace and trim, so break markers don't matter.
std::string norm(const std::string& s) {
  std::string out;
  bool sp = false;
  for (char c : s) {
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      sp = !out.empty();
    } else {
      if (sp) out.push_back(' ');
      sp = false;
      out.push_back(c);
    }
  }
  return out;
}

TestUtils::TestRunner* g_runner = nullptr;

void expectText(const std::string& name, const std::string& src, const std::string& want,
                HtmlStripper::Mode mode = HtmlStripper::Mode::Html) {
  for (size_t chunk : {static_cast<size_t>(0), static_cast<size_t>(1), static_cast<size_t>(3)}) {
    const std::string got = norm(parse(markerize(src, mode, chunk)).text);
    const bool ok = got == want;
    if (!ok) {
      std::fprintf(stderr, "  [%s chunk=%zu]\n    want: '%s'\n    got:  '%s'\n", name.c_str(), chunk,
                   want.c_str(), got.c_str());
    }
    g_runner->expectTrue(ok, (name + "_chunk" + std::to_string(chunk)).c_str());
  }
}

void expectImages(const std::string& name, const std::string& src, const std::vector<std::string>& want,
                  HtmlStripper::Mode mode = HtmlStripper::Mode::Html) {
  const Parsed p = parse(markerize(src, mode, 0));
  const bool ok = p.images == want;
  if (!ok) {
    std::fprintf(stderr, "  [%s] images got %zu:", name.c_str(), p.images.size());
    for (const auto& s : p.images) std::fprintf(stderr, " '%s'", s.c_str());
    std::fprintf(stderr, "\n");
  }
  g_runner->expectTrue(ok, name.c_str());
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("HtmlStripperIntegrity");
  g_runner = &runner;

  // --- Bare ampersands are literal text (pre-fix: everything up to the next
  //     ';' or '<' was swallowed). ---
  expectText("bare_amp_words", "<p>Tom & Jerry went home</p>", "Tom & Jerry went home");
  expectText("bare_amp_acronyms", "<p>AT&T and R&D, Q&A.</p>", "AT&T and R&D, Q&A.");
  expectText("bare_amp_url", "<p>see x.com/?a=1&b=2 now</p>", "see x.com/?a=1&b=2 now");
  expectText("bare_amp_then_semicolon", "<p>salt & pepper; more</p>", "salt & pepper; more");
  expectText("bare_amp_at_end", "<p>end &</p>", "end &");
  expectText("amp_hash_nonnumeric", "<p>a &#; b</p>", "a &#; b");

  // --- Unknown entities are kept literally (pre-fix: silently dropped). ---
  expectText("unknown_entity_literal", "<p>a &bogus; b</p>", "a &bogus; b");
  // Legacy names decode without ';' (as in browsers); others stay literal.
  expectText("legacy_no_semicolon", "<p>a&nbsp;b &copy 2024 &amp more</p>", "a\xC2\xA0" "b \xC2\xA9 2024 & more");
  expectText("nonlegacy_no_semicolon", "<p>this&or that, x&le y</p>", "this&or that, x&le y");
  expectText("cp1252_numeric", "<p>a&#151;b &#147;q&#148; &#133;</p>",
             "a\xE2\x80\x94" "b \xE2\x80\x9Cq\xE2\x80\x9D \xE2\x80\xA6");
  expectText("long_entity_like_run", "<p>&abcdefghijklmnopqrstuvwxyz; end</p>", "&abcdefghijklmnopqrstuvwxyz; end");

  // --- Known entities (incl. ones missing pre-fix). ---
  expectText("entity_basic", "<p>&amp;&lt;&gt;&quot;&apos;</p>", "&<>\"'");
  expectText("entity_numeric", "<p>&#x41;&#66;&#x416;</p>", "AB\xD0\x96");
  expectText("entity_bdquo", "<p>&bdquo;\xD0\x9F\xD1\x80\xD0\xB8&ldquo;</p>",
             "\xE2\x80\x9E\xD0\x9F\xD1\x80\xD0\xB8\xE2\x80\x9C");
  expectText("entity_spanish", "<p>&aacute;&eacute;&iacute;&oacute;&uacute;&ntilde;</p>",
             "\xC3\xA1\xC3\xA9\xC3\xAD\xC3\xB3\xC3\xBA\xC3\xB1");
  expectText("entity_math", "<p>2&minus;1 &le; 3 &infin;</p>", "2\xE2\x88\x92" "1 \xE2\x89\xA4 3 \xE2\x88\x9E");
  expectText("entity_greek", "<p>&alpha;&beta;&Omega;</p>", "\xCE\xB1\xCE\xB2\xCE\xA9");

  // --- A '<' that cannot start a tag is literal (pre-fix: text up to the next
  //     '>' was swallowed). ---
  expectText("stray_lt_space", "<p>5 < 10 and 20 > 15</p>", "5 < 10 and 20 > 15");
  expectText("stray_lt_digit", "<p>x<5 or <3 love</p>", "x<5 or <3 love");

  // --- <head><title> must not leak into the body (pre-fix it was emitted as
  //     the first text of every chapter). ---
  expectText("head_title_skipped",
             "<html><head><title>Book Title</title><meta charset=\"utf-8\"/></head>"
             "<body><p>Body text</p></body></html>",
             "Body text");
  expectText("svg_title_skipped", "<p>a</p><svg><title>Figure 3</title></svg><p>b</p>", "a b");

  // --- Invisible format characters are stripped (pre-fix rendered as '?'). ---
  expectText("soft_hyphen_entity", "<p>\xD1\x80\xD0\xB0\xD0\xB7&shy;\xD0\xBD\xD1\x8B\xD0\xB9</p>",
             "\xD1\x80\xD0\xB0\xD0\xB7\xD0\xBD\xD1\x8B\xD0\xB9");
  expectText("soft_hyphen_literal", "<p>\xD1\x80\xD0\xB0\xD0\xB7\xC2\xAD\xD0\xBD\xD1\x8B\xD0\xB9</p>",
             "\xD1\x80\xD0\xB0\xD0\xB7\xD0\xBD\xD1\x8B\xD0\xB9");
  expectText("zwsp_bom_wj", "\xEF\xBB\xBF<p>a\xE2\x80\x8B" "b\xE2\x81\xA0" "c</p>", "abc");
  // Neighbouring characters that share those lead bytes must pass through.
  expectText("same_lead_bytes_kept",
             "<p>\xC2\xA9 \xC2\xAB\xE2\x80\x94\xE2\x80\xA6\xC2\xBB \xE2\x82\xAC \xEF\xBC\xA1 \xC2\xA0</p>",
             "\xC2\xA9 \xC2\xAB\xE2\x80\x94\xE2\x80\xA6\xC2\xBB \xE2\x82\xAC \xEF\xBC\xA1 \xC2\xA0");

  expectText("unclosed_title_ends_at_head", "<head><title>T</head><body><p>Kept</p></body>", "Kept");
  expectText("control_chars_dropped", "<p>a\x02" "b\x0C" "c\xC2\x81" "d</p>", "ab cd");
  expectText("raw_c1_cp1252", "<p>a\xC2\x97" "b\xC2\x85</p>", "a\xE2\x80\x94" "b\xE2\x80\xA6");

  // --- Self-closing tags close what they open; raw-body tags don't swallow. ---
  {
    const auto m = markerize("<p>a<b/>b</p><p>c</p>", HtmlStripper::Mode::Html, 0);
    size_t on = 0, off = 0;
    for (size_t i = 0; i + 1 < m.size(); ++i) {
      if (m[i] != snapix::smolport::kMarker) continue;
      if (m[i + 1] == snapix::smolport::kBoldOn) ++on;
      if (m[i + 1] == snapix::smolport::kBoldOff) ++off;
      ++i;
    }
    runner.expectTrue(on == 1 && off == 1, "self_closing_b_balanced");
  }
  expectText("self_closing_script", "<head><script src=\"x.js\"/></head><body><p>Text one</p><p>two</p></body>",
             "Text one two");
  expectText("self_closing_title", "<head><title/></head><body><p>Body</p></body>", "Body");
  expectText("self_closing_heading", "<p>a</p><h2/><p>b</p>", "a b");

  // --- Comments / processing instructions. ---
  expectText("comment_with_gt", "<p>a<!-- x > y -->b</p>", "ab");
  expectText("xml_pi_skipped", "<?xml version=\"1.0\"?><p>ok</p>", "ok");
  expectText("doctype_skipped", "<?xml version=\"1.0\"?><!DOCTYPE html><html><body><p>ok</p></body></html>", "ok");
  expectText("end_tag_without_name", "<p>a </ b</>c</p>", "a </ bc");

  // --- FB2 <title> is the chapter title — must stay visible. ---
  expectText("fb2_title_visible", "<section><title><p>Chapter One</p></title><p>Text</p></section>",
             "Chapter One Text", HtmlStripper::Mode::Fb2);

  // --- Images. ---
  expectImages("img_src", "<p><img src=\"a.jpg\"/></p>", {"a.jpg"});
  expectImages("svg_image_xlink", "<svg><image xlink:href=\"../Images/c.jpg\" width=\"1\"/></svg>",
               {"../Images/c.jpg"});
  expectImages("svg_image_href", "<svg><image href=\"x.png\"/></svg>", {"x.png"});
  expectImages("fb2_image", "<image l:href=\"#pic\"/>", {"#pic"}, HtmlStripper::Mode::Fb2);
  expectImages("fb2_image_xlink_prefix", "<image xlink:href=\"#pic2\"/>", {"#pic2"}, HtmlStripper::Mode::Fb2);
  expectImages("img_and_text_after", "<p><img src=\"a.jpg\" alt=\"x > y\"/>after</p>", {"a.jpg"});
  expectText("img_alt_with_gt_text_kept", "<p><img src=\"a.jpg\" alt=\"x > y\"/>after</p>", "after");

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
