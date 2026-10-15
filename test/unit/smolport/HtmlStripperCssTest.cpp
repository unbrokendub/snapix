#include "test_utils.h"

#include <CssStyleTable.h>
#include <HtmlStripper.h>
#include <MarkerizeChapter.h>
#include <Markers.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// =============================================================================
// HtmlStripperCssTest — v3.11.13 EPUB CSS: class / inline-style italic, bold,
// text-align:center, left-margin indent and display:none, resolved per element
// by HtmlStripper against a compiled CssStyleTable.
//
// Every stripper case is fed whole, one byte at a time and in 3-byte reads
// (attribute values, declarations and tags split across reads) and must give
// the same marker bytes.  The property test proves no visible text is lost or
// invented: only text inside display:none elements disappears.
// =============================================================================

using snapix::smolport::CssEffects;
using snapix::smolport::CssStyleSheetCompiler;
using snapix::smolport::CssStyleTable;
using snapix::smolport::HtmlStripper;
using snapix::smolport::markerizeChapter;

namespace {

TestUtils::TestRunner* g_runner = nullptr;

std::vector<uint8_t> compileBlob(const std::vector<std::string>& sheets, size_t chunk = 0) {
  CssStyleSheetCompiler c;
  for (const auto& s : sheets) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    if (chunk == 0) {
      c.feed(p, s.size());
    } else {
      for (size_t i = 0; i < s.size(); i += chunk) c.feed(p + i, std::min(chunk, s.size() - i));
    }
    c.endSheet();
  }
  std::vector<uint8_t> blob;
  c.serialize(blob);
  return blob;
}

void compile(CssStyleTable& t, const std::string& css) {
  const auto blob = compileBlob({css});
  const bool ok = t.load(blob.data(), blob.size());
  if (!ok) std::fprintf(stderr, "  table load failed for: %s\n", css.c_str());
}

uint32_t key(const std::string& selector) {
  uint32_t h = snapix::smolport::kCssHashSeed;
  for (char c : selector) h = snapix::smolport::cssHash(h, static_cast<uint8_t>(c));
  return h;
}

std::string fxStr(const CssEffects fx) {
  std::string s;
  const char* names = "ibcdh";
  for (int i = 0; i < 5; ++i) {
    const uint8_t bit = static_cast<uint8_t>(1u << i);
    if (fx.mask & bit) {
      s.push_back((fx.value & bit) ? static_cast<char>(names[i] - 32) : names[i]);
    }
  }
  return s;  // upper = on, lower = declared off; "" = no rule
}

std::vector<uint8_t> markerize(const std::string& src, const CssStyleTable* styles, size_t chunk,
                               HtmlStripper::Mode mode = HtmlStripper::Mode::Html) {
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
      buf, sizeof(buf), {}, nullptr, styles);
  return out;
}

// Interpret the marker stream the way StreamingPaginator does (boolean bold /
// italic / centre, counted indent) and render each text run as
// `flags:word` (flags: i italic, b bold, c centred, d indented, h heading,
// q quote).  `|` paragraph break, `/` line break, `@id` anchor, `[img]`.
struct Rendered {
  std::string text;   // decorated words
  std::string plain;  // words only (visible text)
  bool balanced = true;
};

Rendered render(const std::vector<uint8_t>& m) {
  Rendered r;
  bool bold = false, italic = false, center = false, heading = false, quote = false;
  int indent = 0;
  std::string run;
  auto flush = [&]() {
    if (run.empty()) return;
    std::string flags;
    if (italic) flags += 'i';
    if (bold) flags += 'b';
    if (center) flags += 'c';
    if (indent > 0) flags += 'd';
    if (heading) flags += 'h';
    if (quote) flags += 'q';
    if (!r.text.empty()) r.text += ' ';
    r.text += flags.empty() ? run : flags + ":" + run;
    if (!r.plain.empty()) r.plain += ' ';
    r.plain += run;
    run.clear();
  };
  auto token = [&](const std::string& t) {
    flush();
    if (!r.text.empty()) r.text += ' ';
    r.text += t;
  };
  for (size_t i = 0; i < m.size(); ++i) {
    const uint8_t b = m[i];
    if (b != snapix::smolport::kMarker) {
      if (b == ' ' || b == '\n' || b == '\t' || b == '\r') flush();
      else run.push_back(static_cast<char>(b));
      continue;
    }
    if (i + 1 >= m.size()) break;
    const uint8_t tag = m[++i];
    using namespace snapix::smolport;
    switch (tag) {
      case kMarker: run.push_back('\x01'); break;
      case kBoldOn: flush(); bold = true; break;
      case kBoldOff: flush(); bold = false; break;
      case kItalicOn: flush(); italic = true; break;
      case kItalicOff: flush(); italic = false; break;
      case kCenterOn: flush(); center = true; break;
      case kCenterOff: flush(); center = false; break;
      case kIndentOn: flush(); ++indent; break;
      case kIndentOff: flush(); if (indent > 0) --indent; break;
      case kQuoteOn: flush(); quote = true; break;
      case kQuoteOff: flush(); quote = false; break;
      case kHeadingOn: flush(); heading = true; ++i; break;
      case kHeadingOff: flush(); heading = false; break;
      case kParagraphBreak: token("|"); break;
      case kLineBreak: token("/"); break;
      case kBreak: token("--"); break;
      case kPageBreak: token("##"); break;
      case kImageRef:
      case kAnchor: {
        if (i + 2 >= m.size()) break;
        const size_t len = m[i + 1] | (static_cast<size_t>(m[i + 2]) << 8);
        const std::string payload(reinterpret_cast<const char*>(m.data() + i + 3),
                                  std::min(len, m.size() - (i + 3)));
        token(tag == kAnchor ? "@" + payload : "[" + payload + "]");
        i += 2 + len;
        break;
      }
      default: flush(); break;
    }
  }
  flush();
  r.balanced = !bold && !italic && !center && indent == 0;
  return r;
}

// Same marker bytes whatever the read size; then compare the rendering.
void expectRender(const std::string& name, const CssStyleTable* t, const std::string& src,
                  const std::string& want) {
  const auto whole = markerize(src, t, 0);
  bool sameBytes = true;
  for (size_t chunk : {static_cast<size_t>(1), static_cast<size_t>(3)}) {
    if (markerize(src, t, chunk) != whole) {
      sameBytes = false;
      std::fprintf(stderr, "  [%s] chunk=%zu changed the marker bytes\n", name.c_str(), chunk);
    }
  }
  const Rendered r = render(whole);
  const bool ok = r.text == want;
  if (!ok) std::fprintf(stderr, "  [%s]\n    want: '%s'\n    got:  '%s'\n", name.c_str(), want.c_str(), r.text.c_str());
  g_runner->expectTrue(ok && sameBytes, name);
}

// ---------------------------------------------------------------------------
// Compiler.
// ---------------------------------------------------------------------------
void testCompiler() {
  const std::string css =
      "@charset \"utf-8\";\n"
      "@import url(\"other.css\");\n"
      "/* comment { .italic { font-weight: bold } } */\n"
      ".italic { font-style: italic }\n"
      ".obl{font-style:oblique;}\n"
      "span.b7 { font-weight: 700 } .b5 { font-weight: 500 } .bolder { font-weight: bolder !important }\n"
      "P.Note, .Center ,h2.x { text-align: center; }\n"
      ".pagenum { display: none } .shown { display: block }\n"
      ".quote { margin-left: 2em } .tiny { margin-left: 0.5em } .m2 { margin: 0 30px } .m4 { margin: 0 0 0 5% } .m3 { margin: 0 0 0 3% }\n"
      ".mneg { margin-left: -2em } .mauto { margin: 0 auto }\n"
      ".sh { font: italic bold 12px/1.4 \"Times New Roman\", serif } .shn { font: 1em serif } .shi { font: inherit }\n"
      "div .desc, a:hover, #id, .a.b, *, p > span, [lang] , .esc\\:x, p.x:first-child { font-style: italic }\n"
      "@media amzn-kf8 { .kindle { display: none } }\n"
      "@font-face { font-family: X; src: url(x.ttf) }\n"
      ".str { content: \"};{\"; font-weight: bold }\n"
      "blockquote { font-style: italic }\n"
      ".unclosed { font-weight: bold";
  for (size_t chunk : {static_cast<size_t>(0), static_cast<size_t>(1), static_cast<size_t>(5)}) {
    CssStyleTable t;
    const auto blob = compileBlob({css}, chunk);
    g_runner->expectTrue(t.load(blob.data(), blob.size()), "compiler_table_loads_chunk" + std::to_string(chunk));
    auto check = [&](const std::string& sel, const std::string& want) {
      const std::string got = fxStr(t.lookup(key(sel)));
      if (got != want) {
        std::fprintf(stderr, "  [chunk=%zu] '%s' want '%s' got '%s'\n", chunk, sel.c_str(), want.c_str(),
                     got.c_str());
      }
      return got == want;
    };
    bool ok = true;
    ok &= check(".italic", "I");
    ok &= check(".obl", "I");
    ok &= check("span.b7", "B");
    ok &= check(".b7", "");  // tag.class only
    ok &= check(".bolder", "B");
    ok &= check("p.Note", "C");  // tag lower-cased, class verbatim
    ok &= check("p.note", "");
    ok &= check(".Center", "C");
    ok &= check("h2.x", "C");
    ok &= check(".pagenum", "H");
    ok &= check(".shown", "h");  // kept: something is display:none
    ok &= check(".quote", "D");
    ok &= check(".m2", "D");
    ok &= check(".m4", "D");
    ok &= check(".m3", "d");
    ok &= check(".sh", "IB");
    ok &= check(".shi", "");
    ok &= check(".str", "B");
    ok &= check("blockquote", "I");
    ok &= check(".unclosed", "B");
    // Unsupported selectors and at-rule bodies never produce entries.
    for (const char* sel : {".desc", "div", "a", "#id", ".a", ".b", "span", "p", ".esc", ".kindle", ".x"}) {
      ok &= check(sel, "");
    }
    g_runner->expectTrue(ok, "compiler_selectors_and_properties_chunk" + std::to_string(chunk));
  }

  // Values that are not "on" are declared off.  Indent / display off-
  // declarations are pruned while no rule turns them on; italic / bold /
  // centre ones always stay (they suspend tags and ancestors).
  {
    CssStyleTable t;
    const std::string offs =
        ".b5 { font-weight: 500 } .tiny { margin-left: 0.5em } .mneg { margin-left: -2em } "
        ".mauto { margin: 0 auto } .shn { font: 1em serif } .left { text-align: left } .shown { display: block }";
    compile(t, offs);
    bool ok = t.size() == 3 && fxStr(t.lookup(key(".b5"))) == "b" && fxStr(t.lookup(key(".shn"))) == "ib" &&
              fxStr(t.lookup(key(".left"))) == "c" && fxStr(t.lookup(key(".tiny"))) == "" &&
              fxStr(t.lookup(key(".shown"))) == "";
    g_runner->expectTrue(ok, "compiler_prunes_unused_indent_display_off");
    compile(t, ".on { margin-left: 2em } .gone { display: none } " + offs);
    ok = fxStr(t.lookup(key(".tiny"))) == "d" && fxStr(t.lookup(key(".mneg"))) == "d" &&
         fxStr(t.lookup(key(".mauto"))) == "d" && fxStr(t.lookup(key(".shown"))) == "h";
    g_runner->expectTrue(ok, "compiler_off_values_kept_when_overriding");
  }


  // Later declarations of the same selector win; several sheets merge.
  {
    CssStyleTable t;
    const auto blob = compileBlob({".x { font-style: italic } .y { font-weight: bold }",
                                   ".x { font-style: normal; font-weight: bold } .z { font-style: italic"});
    t.load(blob.data(), blob.size());
    const bool ok = fxStr(t.lookup(key(".x"))) == "iB" && fxStr(t.lookup(key(".y"))) == "B" &&
                    fxStr(t.lookup(key(".z"))) == "I";
    g_runner->expectTrue(ok, "compiler_later_rules_win_across_sheets");
  }

  // Malformed blobs are rejected and leave an empty table.
  {
    CssStyleTable t;
    auto blob = compileBlob({".a{font-style:italic} .b{font-weight:bold}"});
    bool ok = t.load(blob.data(), blob.size()) && t.size() == 2;
    auto bad = blob;
    bad[2] = 99;  // version
    ok &= !t.load(bad.data(), bad.size()) && t.size() == 0;
    bad = blob;
    bad.pop_back();  // truncated
    ok &= !t.load(bad.data(), bad.size());
    bad = blob;
    std::swap_ranges(bad.begin() + 6, bad.begin() + 12, bad.begin() + 12);  // unsorted
    ok &= !t.load(bad.data(), bad.size());
    ok &= !t.load(nullptr, 0) && t.lookup(key(".a")).mask == 0;
    g_runner->expectTrue(ok, "table_rejects_malformed_blobs");
  }

  // Inline declaration parser: comments, strings, functions, !important.
  {
    auto inl = [](const std::string& s) {
      snapix::smolport::CssDeclParser p;
      for (char c : s) p.feed(static_cast<uint8_t>(c));
      return fxStr(p.finish());
    };
    bool ok = inl("font-style:italic") == "I" && inl(" FONT-WEIGHT : Bold ; ") == "B" &&
              inl("color:red;font-style:italic") == "I" &&
              inl("font-family:\"a;b\"; font-weight:600") == "B" &&
              inl("background:url(a;b.png); text-align:center") == "C" &&
              inl("/* x:y; */display:none") == "H" && inl("font-style: italic !important") == "I" &&
              inl("font-style:italic; font-style:normal") == "i" && inl("font-weight:inherit") == "" &&
              inl("margin:1em 2em 1em") == "D" && inl("margin:2em 0 0 0") == "d" &&
              inl("margin-left:calc(2em)") == "d" && inl("unknown:italic") == "" &&
              inl("font-style") == "" && inl("font: bold 1em/1.2 serif") == "iB";
    g_runner->expectTrue(ok, "decl_parser_inline_styles");
  }
}

// ---------------------------------------------------------------------------
// Stripper.
// ---------------------------------------------------------------------------
const char* const kSheet =
    ".italic { font-style: italic } .bold { font-weight: bold } .center { text-align: center }"
    ".hidden, .pagenum { display: none } .quote { margin-left: 2.5em } .normal { font-style: normal }"
    "p.note { font-style: italic } .x { font-weight: bold } p.x { font-weight: normal }"
    "cite { font-style: italic } h1 { font-weight: bold }";

void testStripper() {
  CssStyleTable t;
  compile(t, kSheet);

  // --- Class / inline italic + bold. ---
  expectRender("class_italic_span", &t, "<p>a <span class=\"italic\">b c</span> d</p>", "a i:b i:c d |");
  expectRender("class_bold_div", &t, "<div class=\"bold\">a</div><p>b</p>", "b:a | b |");
  expectRender("class_list_two_classes", &t, "<p>a <span class=\"foo italic  bold\">b</span></p>", "a ib:b |");
  expectRender("inline_italic", &t, "<p>a <span style=\"font-style: italic\">b</span> c</p>", "a i:b c |");
  expectRender("inline_bold_numeric", &t, "<p><span style=\"font-weight:700\">a</span> "
                                           "<span style='font-weight:500'>b</span></p>",
               "b:a b |");
  expectRender("inline_font_shorthand", &t, "<p><span style=\"font: italic bold 1em serif\">a</span> b</p>",
               "ib:a b |");
  expectRender("unquoted_class", &t, "<p><span class=italic>a</span> b</p>", "i:a b |");
  expectRender("tag_selector", &t, "<p><cite>Title</cite> x</p><h1>Head</h1>", "i:Title x | | bch:Head |");

  // --- Italic sources nest: inner tags / spans do not end an outer italic. ---
  expectRender("css_italic_contains_em", &t, "<p class=\"italic\">a <em>b</em> c</p> d",
               "i:a i:b i:c | d");
  expectRender("em_contains_css_italic", &t, "<p><i>a <span class=\"italic\">b</span> c</i> d</p>",
               "i:a i:b i:c d |");
  expectRender("bold_tag_in_bold_class", &t, "<p class=\"bold\"><b>a</b> b</p>", "b:a b:b |");
  expectRender("nested_same_tag", &t, "<p><i>a <i>b</i> c</i> d</p>", "i:a i:b i:c d |");

  // --- Specificity: tag < .class < tag.class < inline. ---
  expectRender("tag_class_beats_class", &t, "<p class=\"x\">a</p><div class=\"x\">b</div>", "a | b:b |");
  expectRender("tag_class_only_on_tag", &t, "<p class=\"note\">a</p><div class=\"note\">b</div>", "i:a | b |");
  expectRender("inline_beats_class", &t, "<p><span class=\"italic\" style=\"font-style:normal\">a</span> b</p>",
               "a b |");
  // --- An element's own "off" suspends the style until its close. ---
  expectRender("off_class_suspends_ancestor", &t,
               "<p class=\"italic\">a <span class=\"normal\">b <i>c</i></span> d</p>", "i:a b i:c i:d |");
  expectRender("off_class_overrides_own_tag", &t, "<p><i class=\"normal\">a</i> <em>b</em></p>", "a i:b |");
  expectRender("off_inline_inside_em", &t, "<p><em>a <span style=\"font-style:normal\">b</span> c</em></p>",
               "i:a b i:c |");
  expectRender("off_bold_in_bold", &t, "<p class=\"bold\">a <span style=\"font-weight:400\">b</span> c</p>",
               "b:a b b:c |");
  expectRender("left_p_in_centered_div", &t,
               "<div class=\"center\"><p style=\"text-align:justify\">long body</p><h2>T</h2></div>x",
               "long body | | ch:T | | x");
  expectRender("left_heading", &t, "<h2 style=\"text-align:left\">T</h2>x", "| h:T | x");
  expectRender("left_span_does_not_uncenter", &t,
               "<p class=\"center\">a <span style=\"text-align:left\">b</span></p>", "c:a c:b |");
  expectRender("suspend_with_unclosed_inner", &t,
               "<p class=\"italic\"><span class=\"normal\">a<i>b</span>c</p>d</i>e", "a i:bc | i:d e");

  // --- text-align:center only on block elements. ---
  expectRender("center_p", &t, "<p>a</p><p class=\"center\">b c</p><p>d</p>", "a | c:b c:c | d |");
  expectRender("center_inline_style_div", &t, "<div style=\"text-align:center\">a</div>b", "c:a | b");
  expectRender("center_ignored_on_span", &t, "<p>a <span class=\"center\">b</span></p>", "a b |");
  expectRender("center_inside_center_tag", &t, "<center><p class=\"center\">a</p>b</center>c",
               "| c:a | c:b | c");
  expectRender("center_heading_class", &t, "<h2 class=\"center\">T</h2>x", "| ch:T | x");

  // --- Left-margin indent (blockquote-like). ---
  expectRender("indent_class", &t, "<p>a</p><p class=\"quote\">b</p><p>c</p>", "a | d:b | c |");
  expectRender("indent_does_not_compound", &t, "<div class=\"quote\"><p class=\"quote\">a</p>b</div>c",
               "d:a | d:b | c");
  expectRender("indent_not_on_blockquote", &t, "<blockquote class=\"quote\"><p>a</p></blockquote>",
               "dq:a |");
  expectRender("indent_inline_margin", &t, "<p style=\"margin: 0 3em\">a</p><p style=\"margin-left:4px\">b</p>",
               "d:a | b |");

  // --- display:none. ---
  expectRender("hidden_span", &t, "<p>keep <span class=\"hidden\">drop <b>more</b></span> after</p>",
               "keep after |");
  expectRender("hidden_nested_same_name", &t,
               "<p>a <span class=\"hidden\">x<span>y</span>z<span><span>w</span></span>v</span> b</p>",
               "a b |");
  expectRender("hidden_div_tree", &t,
               "<div class=\"hidden\"><div><p>p1</p></div><p>p2</p>tail</div><p>visible</p>", "visible |");
  expectRender("hidden_inline_style", &t, "<p>a<span style=\"display:none\">b</span>c</p>", "ac |");
  expectRender("hidden_root_close_markers_dropped", &t, "<p>a</p><p class=\"hidden\">b</p><p>c</p>",
               "a | c |");
  expectRender("hidden_with_void_and_raw", &t,
               "<p class=\"hidden\">a<br>b<img src=\"x.jpg\">c&amp;<script>if(a<b){}</script>"
               "<hr/>d</p><p>e</p>",
               "e |");
  expectRender("hidden_keeps_anchors", &t,
               "<p>a<span class=\"pagenum\" id=\"p5\">5</span>b<span class=\"hidden\"><a id=\"n1\">x</a></span>c</p>",
               "a @p5 b @n1 c |");
  expectRender("hidden_styles_inside_balanced", &t,
               "<p><span class=\"hidden\"><i>a</i><span class=\"bold\">b</span></span>c</p>", "c |");
  expectRender("hidden_misnested_close", &t, "<p><span class=\"hidden\">a<b>b</span>c</b>d</p>", "c d |");
  expectRender("hidden_unclosed_p_closed_by_p", &t, "<body><p class=\"hidden\">x<p>y</p></body>", "y |");
  expectRender("hidden_ends_at_ancestor_close", &t, "<div><p><span class=\"hidden\">x</div>y", "| y");
  // <p> directly inside <p>: closed HTML-style by the inner start tag.  For
  // (invalid) nested-p XHTML that FAILS OPEN — hidden text shows, none is lost.
  expectRender("nested_p_fails_open", &t,
               "<p class=\"hidden\">a<p>b</p></p><p>c</p>", "b | | c |");
  expectRender("hidden_void_self", &t, "<p>a<br class=\"hidden\">b<img class=\"hidden\"/>c<br>d</p>", "abc / d |");
  expectRender("stray_close_ignored", &t, "<p>a</span> <span class=\"italic\">b</span></p>", "a i:b |");

  // --- Unknown classes are a no-op: same bytes as without the class. ---
  {
    const std::string with =
        "<p class=\"calibre1\">a <span class=\"none of these\">b</span> <i class=\"zz\">c</i></p>"
        "<div class=\"q\"><h2 class=\"ch\">T</h2></div>";
    const std::string without = "<p>a <span>b</span> <i>c</i></p><div><h2>T</h2></div>";
    g_runner->expectTrue(markerize(with, &t, 0) == markerize(without, &t, 0), "unknown_classes_no_op");
    // CSS on with no matching rules = CSS off, for well-nested markup.
    g_runner->expectTrue(markerize(with, &t, 0) == markerize(without, nullptr, 0), "unknown_classes_match_css_off");
  }

  // --- An empty table still honours inline styles. ---
  {
    CssStyleTable empty;
    expectRender("empty_table_inline_style", &empty, "<p><span class=\"italic\">a</span> <b style=\"font-style:italic\">b</b></p>",
                 "a ib:b |");
  }

  // --- FB2 mode never resolves CSS. ---
  {
    const std::string fb2 = "<section><p><emphasis>a</emphasis> <span class=\"hidden\">b</span>"
                            "<p style=\"display:none\">c</p></p></section>";
    g_runner->expectTrue(markerize(fb2, &t, 0, HtmlStripper::Mode::Fb2) ==
                             markerize(fb2, nullptr, 0, HtmlStripper::Mode::Fb2),
                         "fb2_mode_ignores_css");
  }

  // --- Deeper than the element stack: CSS off for the overflow, never hides. ---
  {
    std::string src = "<p class=\"hidden\">gone</p>";
    for (int i = 0; i < 40; ++i) src += "<div>";
    src += "<span class=\"hidden\">deep</span><span class=\"italic\">it</span>";
    for (int i = 0; i < 40; ++i) src += "</div>";
    src += "<p>a<span class=\"hidden\">x</span>b</p>";
    const Rendered r = render(markerize(src, &t, 0));
    const bool ok = r.plain.find("gone") == std::string::npos && r.plain.find("deep") != std::string::npos &&
                    r.text.find("i:it") == std::string::npos && r.plain.find("it") != std::string::npos &&
                    r.plain.find("ab") != std::string::npos && r.plain.find('x') == std::string::npos && r.balanced;
    if (!ok) std::fprintf(stderr, "  [depth_overflow] %s\n", r.text.c_str());
    g_runner->expectTrue(ok, "depth_overflow_fails_open_and_recovers");
  }

  // --- reset() clears the element stack and the hidden state. ---
  {
    struct Sink : snapix::smolport::HtmlStripperSink {
      std::vector<uint8_t> out;
      void emit(const uint8_t* d, size_t n) override { out.insert(out.end(), d, d + n); }
    } sink;
    HtmlStripper s(sink, HtmlStripper::Mode::Html, &t);
    const std::string a = "<div class=\"hidden\"><i>x";
    s.feed(reinterpret_cast<const uint8_t*>(a.data()), a.size());
    s.reset();
    sink.out.clear();
    const std::string b = "<p>y <i>z</i></p>";
    s.feed(reinterpret_cast<const uint8_t*>(b.data()), b.size());
    s.finish();
    g_runner->expectTrue(render(sink.out).text == "y i:z |", "reset_clears_css_state");
  }
}

// ---------------------------------------------------------------------------
// Property: no visible text lost, nothing invented.  Random nested documents;
// the oracle knows which words sit under a display:none element.
// ---------------------------------------------------------------------------
struct DocGen {
  std::mt19937 rng;
  int n = 0;
  int rnd(int lo, int hi) { return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)); }

  std::string word() { return "w" + std::to_string(++n); }

  std::string classAttr(bool& hides) {
    static const char* kClasses[] = {"italic", "bold", "center", "quote", "calibre3", "normal", "x", "note"};
    std::string cls;
    const int k = rnd(0, 2);
    for (int i = 0; i < k; ++i) {
      if (!cls.empty()) cls += ' ';
      cls += kClasses[rnd(0, 7)];
    }
    if (rnd(0, 9) == 0) {
      cls += cls.empty() ? "hidden" : " hidden";
      hides = true;
    }
    std::string attrs;
    if (!cls.empty()) attrs += " class=\"" + cls + "\"";
    switch (rnd(0, 12)) {
      case 0: attrs += " style=\"font-style:italic\""; break;
      case 1: attrs += " style=\"font-weight: bold; text-align: center\""; break;
      case 2:
        attrs += " style=\"display:none\"";
        hides = true;
        break;
      case 3: attrs += " id=\"a" + std::to_string(n) + "\""; break;
      default: break;
    }
    return attrs;
  }

  // Appends source to `src`, visible words to `vis`.
  void node(std::string& src, std::string& vis, int depth, bool hidden, bool inP = false) {
    static const char* kTags[] = {"p", "div", "span", "i", "b", "em", "strong", "section", "blockquote", "a", "h2"};
    const int items = rnd(1, depth > 6 ? 2 : 5);
    for (int it = 0; it < items; ++it) {
      const int kind = rnd(0, 9);
      if (kind <= 3 || depth > 9) {
        const std::string w = word();
        src += w + " ";
        if (!hidden) vis += w + " ";
      } else if (kind == 4) {
        src += rnd(0, 1) ? "<br/>" : "<br>";
      } else if (kind == 5) {
        src += "&amp;";
        if (!hidden) vis += "& ";
        src += " ";
      } else {
        const char* tag = kTags[rnd(0, 10)];
        // No <p> directly in <p>: the HTML-style implicit close (see
        // nested_p_fails_open) would not match this XML-nesting oracle.
        if (inP && std::strcmp(tag, "p") == 0) tag = "span";
        bool hides = false;
        const std::string attrs = classAttr(hides);
        src += std::string("<") + tag + attrs + ">";
        node(src, vis, depth + 1, hidden || hides, std::strcmp(tag, "p") == 0);
        src += std::string("</") + tag + "> ";
      }
    }
  }
};

std::string norm(const std::string& s) {
  std::string out;
  bool sp = false;
  for (char c : s) {
    if (c == ' ') {
      sp = !out.empty();
    } else {
      if (sp) out.push_back(' ');
      sp = false;
      out.push_back(c);
    }
  }
  return out;
}

void testNoTextLoss() {
  CssStyleTable t;
  compile(t, kSheet);
  DocGen g;
  g.rng.seed(20261015u);
  int failures = 0, docs = 0, hiddenDocs = 0;
  for (int d = 0; d < 1500; ++d) {
    std::string src = "<html><head><title>T</title><link rel=\"stylesheet\" href=\"s.css\"/></head><body>";
    std::string vis;
    g.node(src, vis, 0, false);
    src += "</body></html>";
    ++docs;
    if (src.find("hidden") != std::string::npos || src.find("display:none") != std::string::npos) ++hiddenDocs;
    const auto whole = markerize(src, &t, 0);
    const Rendered r = render(whole);
    // Words only: anchors / breaks / images are not text.
    std::string words;
    for (size_t i = 0; i < r.plain.size(); ++i) words.push_back(r.plain[i]);
    const bool same = norm(words) == norm(vis);
    const bool chunkSame = markerize(src, &t, 1 + (d % 7)) == whole;
    if (!same || !chunkSame || !r.balanced) {
      if (++failures <= 3) {
        std::fprintf(stderr, "  [no_text_loss doc=%d] same=%d chunk=%d balanced=%d\n  src: %s\n  want: %s\n  got:  %s\n",
                     d, same, chunkSame, r.balanced, src.c_str(), norm(vis).c_str(), norm(words).c_str());
      }
    }
  }
  std::fprintf(stderr, "  no_text_loss docs=%d (with hidden=%d) failures=%d\n", docs, hiddenDocs, failures);
  g_runner->expectTrue(failures == 0 && hiddenDocs > 100, "property_only_hidden_text_disappears");
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("HtmlStripperCss");
  g_runner = &runner;
  testCompiler();
  testStripper();
  testNoTextLoss();
  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
