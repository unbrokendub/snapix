#include "test_utils.h"

#include <ChunkedMarkerizer.h>
#include <CssStyleTable.h>
#include <HtmlStripper.h>
#include <MarkerizeChapter.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

// =============================================================================
// EpubChunkEquivTest — v3.11.12 progressive markerize of big EPUB chapters.
//
// The firmware (EpubChapterParser::markerizeProgressiveChunks) stores a chapter
// as chunk segments cut at plain SOURCE offsets (MarkerChunkPlan), with ONE
// HtmlStripper alive across them.  Every parsePages call reopens the ZIP stream
// and either SKIPS the inflated prefix (live stripper) or REPLAYS it through a
// fresh stripper (parser reset / reboot), then markerizes 1..N chunks.
//
// Property: the concatenated chunk segments are byte-identical to one
// continuous markerizeChapter() pass — for any chunk size (cuts inside tags,
// attributes, entities, comments, <script>/<style>/<title> bodies, UTF-8
// sequences), any read granularity, and any mix of skip / replay calls.
//
// v3.11.13 — the same holds with the book's CSS table on (element stack,
// display:none subtrees and counted styles live in the stripper and are
// rebuilt by replay).
// =============================================================================

using snapix::smolport::ChunkedMarkerizer;
using snapix::smolport::CssStyleTable;
using snapix::smolport::HtmlStripper;
using snapix::smolport::MarkerChunkPlan;
using snapix::smolport::MarkerizeReadFn;
using snapix::smolport::MarkerizeStatus;
using snapix::smolport::MarkerizeWriteFn;

namespace {

// Sequential source over `doc` from byte 0 (a freshly opened ZIP stream) that
// returns random-sized reads.
MarkerizeReadFn makeSource(const std::string& doc, std::mt19937& rng, size_t maxRead) {
  auto pos = std::make_shared<size_t>(0);
  auto local = std::make_shared<std::mt19937>(rng());
  return [&doc, pos, local, maxRead](uint8_t* buf, size_t len) -> int {
    if (*pos >= doc.size()) return 0;
    size_t n = std::min(len, doc.size() - *pos);
    if (maxRead > 0) n = std::min(n, 1 + (*local)() % maxRead);
    std::memcpy(buf, doc.data() + *pos, n);
    *pos += n;
    return static_cast<int>(n);
  };
}

// Reference: the one-shot device path (markerizeChapter, 4 KB reads).
std::string onePass(const std::string& doc, const CssStyleTable* styles) {
  std::string out;
  size_t pos = 0;
  std::vector<uint8_t> buf(4096);
  markerizeChapter(
      HtmlStripper::Mode::Html,
      [&](uint8_t* b, size_t n) -> int {
        if (pos >= doc.size()) return 0;
        const size_t k = std::min(n, doc.size() - pos);
        std::memcpy(b, doc.data() + pos, k);
        pos += k;
        return static_cast<int>(k);
      },
      [&](const uint8_t* d, size_t n) -> bool {
        out.append(reinterpret_cast<const char*>(d), n);
        return true;
      },
      buf.data(), buf.size(), {}, nullptr, styles);
  return out;
}

struct ChunkedResult {
  std::vector<std::string> segments;
  int replays = 0;
  int skips = 0;
  bool ok = true;
};

// Mirror of the firmware loop.  Each "call" opens a fresh source at byte 0,
// positions the stripper (replay when cold, skip when warm), then markerizes
// 1..3 chunks in a row from the same open stream.
ChunkedResult chunked(const std::string& doc, const MarkerChunkPlan& plan, std::mt19937& rng, int coldPct,
                      size_t maxRead, size_t stageSize, const CssStyleTable* styles) {
  ChunkedResult r;
  const uint32_t size = static_cast<uint32_t>(doc.size());
  const int total = plan.count(size);
  std::unique_ptr<ChunkedMarkerizer> m;
  std::vector<uint8_t> buf(1 + rng() % 700);
  std::vector<uint8_t> stage(stageSize);
  int k = 0;
  while (k < total) {
    if (m && static_cast<int>(rng() % 100) < coldPct) m.reset();  // parser reset / reboot
    MarkerizeReadFn src = makeSource(doc, rng, maxRead);
    const uint32_t start = plan.start(k, size);
    if (!m) {
      m.reset(new ChunkedMarkerizer(HtmlStripper::Mode::Html, styles));
      if (start > 0) {
        ++r.replays;
        if (!m->replayTo(src, start, buf.data(), buf.size())) {
          r.ok = false;
          return r;
        }
      }
    } else {
      ++r.skips;
      if (m->offset() != start || !snapix::smolport::skipSource(src, start, buf.data(), buf.size())) {
        r.ok = false;
        return r;
      }
    }
    const int batch = 1 + static_cast<int>(rng() % 3);
    for (int b = 0; b < batch && k < total; ++b, ++k) {
      std::string seg;
      const MarkerizeWriteFn write = [&seg](const uint8_t* d, size_t n) -> bool {
        seg.append(reinterpret_cast<const char*>(d), n);
        return true;
      };
      const MarkerizeStatus st = m->markerizeChunk(src, write, plan.end(k, size), k == total - 1, buf.data(),
                                                   buf.size(), stage.data(), stage.size());
      if (st != MarkerizeStatus::Success || m->offset() != plan.end(k, size)) {
        r.ok = false;
        return r;
      }
      r.segments.push_back(std::move(seg));
    }
  }
  return r;
}

std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += x;
  return s;
}

// ---------------------------------------------------------------------------
// Documents.
// ---------------------------------------------------------------------------
const char* const kFixedDocs[] = {
    // Entities (named, numeric, cp1252 remap, bogus, bare '&'), multi-byte UTF-8.
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n<html><head><title>Title text</title>"
    "<style type=\"text/css\">p > q { margin: 0 }</style></head><body>"
    "<p>Tom &amp; Jerry &nbsp; went &#1074;&#x434;&#X43E;&#151; home &bogus; &amp &lt;tag&gt; Tom & Jerry</p>"
    "<p>Кириллица \xF0\x9F\x98\x80 emoji \xE2\x80\x8B zwsp \xC2\xAD shy</p></body></html>",
    // Raw bodies with '<' / '>' / fake closers, comments, attributes holding '>'.
    "<html><body><script>if (a < b && c > d) { x = \"</p>\"; }</script>"
    "<!-- comment with <b>tags</b> and > signs -- -->"
    "<p title=\"x > y\" class='a<b'>Quoted &quot;attrs&quot;</p>"
    "<a id=\"note1\" href=\"#n\">anchor</a><img src=\"images/pic%201.png\" alt=\"a > b\"/>"
    "<svg><desc>skip me</desc><image xlink:href=\"img/s.jpg\"/><metadata>m</metadata></svg>"
    "<p>after svg<b/>self<br/>closing<i/>tags</p><style/><p>after empty style</p></body></html>",
    // Block structure: headings, lists, tables, quotes, center, hr, stray '<'.
    "<body><h1>Chapter <em>One</em></h1><h2>Sub</h2><blockquote><p>Quoted</p></blockquote>"
    "<ul><li>one</li><li>two <strong>bold</strong></li></ul><ol><li>x</li></ol>"
    "<table><tr><td>c1</td><td>c2</td></tr></table><center>mid</center><hr/>"
    "<p>a < b and 3<4 but <notatag and x<y</p><dl><dt>t</dt><dd>d</dd></dl>"
    "<p>sup<sup>1</sup> sub<sub>2</sub></p><section><p>sec</p></section></body>",
};

struct Gen {
  std::mt19937 rng;
  int rnd(int lo, int hi) { return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)); }

  std::string word() {
    std::string w;
    const int n = rnd(1, 12);
    for (int i = 0; i < n; ++i) {
      const int r = rnd(1, 100);
      if (r <= 15) {
        const int cp = 0x430 + rnd(0, 31);
        w.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        w.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      } else {
        w.push_back(static_cast<char>('a' + rnd(0, 25)));
      }
    }
    return w;
  }

  std::string fragment() {
    switch (rnd(0, 24)) {
      case 0: return "&amp;";
      case 1: return "&nbsp;";
      case 2: return "&#" + std::to_string(rnd(32, 1200)) + ";";
      case 3: return "&#x" + std::string(1, "0123456789abcdef"[rnd(2, 15)]) + "1;";
      case 4: return " & ";
      case 5: return "<b>" + word() + "</b>";
      case 6: return "<i>" + word() + "</i>";
      case 7: return "<a id=\"id" + std::to_string(rnd(1, 999)) + "\"></a>";
      case 8: return "<img src=\"images/i" + std::to_string(rnd(1, 99)) + ".jpg\"/>";
      case 9: return "<br/>";
      case 10: return "<!-- c " + word() + " > x -->";
      case 11: return "<span class=\"s\">" + word() + "</span>";
      case 12: return "<script>var s = '<p>' + 1 < 2;</script>";
      case 13: return "<style>p>b{x:1}</style>";
      case 14: return " a<b ";
      case 15: return "<sup>" + std::to_string(rnd(1, 9)) + "</sup>";
      case 16: return "\xE2\x80\x8B";  // ZWSP (dropped)
      case 17: return "\xF0\x9F\x93\x96";  // 4-byte UTF-8
      case 18: return "<p title=\"q>r\">" + word() + "</p>";
      case 19: return "<b/>";
      case 20: return "<span class=\"it h\">" + word() + " <span>" + word() + "</span></span>";
      case 21: return "<span style=\"display:none\">" + word() + "<br>" + word() + "</span>";
      case 22: return "<em class=\"bd\" style=\"font-style: normal; font-weight:700\">" + word() + "</em>";
      default: return word();
    }
  }

  std::string block() {
    std::string s;
    const int n = rnd(1, 30);
    for (int i = 0; i < n; ++i) {
      if (i) s += rnd(0, 5) == 0 ? "\n  " : " ";
      s += fragment();
    }
    switch (rnd(0, 9)) {
      case 0: return "<h" + std::to_string(rnd(1, 3)) + ">" + s + "</h" + std::to_string(rnd(1, 3)) + ">";
      case 8: return "<p class=\"c q\">" + s + "</p>";
      case 9: return "<div class=\"h\"><p>" + s + "</p></div>";
      case 1: return "<blockquote><p>" + s + "</p></blockquote>";
      case 2: return "<ul><li>" + s + "</li></ul>";
      case 3: return "<div>" + s + "</div>";
      case 4: return "<p>" + s;  // unclosed
      default: return "<p>" + s + "</p>\n";
    }
  }

  std::string doc() {
    std::string s = "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>" + word() +
                    "</title><style>p{}</style></head><body>";
    const int n = rnd(1, 40);
    for (int i = 0; i < n; ++i) s += block();
    return s + "</body></html>";
  }
};

void testPlan(TestUtils::TestRunner& runner) {
  bool ok = true;
  for (uint32_t size : {0u, 1u, 7u, 100u, 32768u, 32769u, 65536u, 418000u}) {
    for (const MarkerChunkPlan plan : {MarkerChunkPlan{32768, 240}, MarkerChunkPlan{100, 3}, MarkerChunkPlan{7, 1}}) {
      const int n = plan.count(size);
      uint32_t expect = 0;
      for (int k = 0; k < n; ++k) {
        ok = ok && plan.start(k, size) == expect && plan.end(k, size) >= expect;
        expect = plan.end(k, size);
      }
      ok = ok && expect == size && n >= 1 && n <= plan.maxChunks;
    }
  }
  runner.expectTrue(ok, "plan_chunks_tile_source_exactly");
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("EpubChunkEquiv");
  testPlan(runner);

  std::vector<std::string> docs;
  for (const char* d : kFixedDocs) docs.emplace_back(d);
  {
    Gen g;
    g.rng.seed(20261015u);
    for (int i = 0; i < 120; ++i) docs.push_back(g.doc());
    // A few big ones (hundreds of KB, like the device repro).
    for (int i = 0; i < 3; ++i) {
      std::string big;
      while (big.size() < 300000) big += g.doc();
      docs.push_back(big);
    }
  }

  // v3.11.13 — the whole matrix runs twice: CSS off, then with a book table.
  CssStyleTable css;
  {
    const std::string sheet =
        ".it { font-style: italic } .h { display: none } .c { text-align: center } .q { margin-left: 3em }"
        ".bd { font-weight: bold } .s { font-weight: bold } p { font-style: normal }";
    snapix::smolport::CssStyleSheetCompiler compiler;
    compiler.feed(reinterpret_cast<const uint8_t*>(sheet.data()), sheet.size());
    compiler.endSheet();
    std::vector<uint8_t> blob;
    compiler.serialize(blob);
    runner.expectTrue(css.load(blob.data(), blob.size()) && css.size() == 7, "css_table_built");
  }
  int cssChanged = 0;
  for (const auto& doc : docs) cssChanged += onePass(doc, &css) != onePass(doc, nullptr) ? 1 : 0;
  runner.expectTrue(cssChanged > 50, "css_changes_markers_of_many_docs");

  std::mt19937 rng(7);
  int cases = 0, failures = 0, replays = 0, skips = 0;
  for (const CssStyleTable* styles : {static_cast<const CssStyleTable*>(nullptr), static_cast<const CssStyleTable*>(&css)}) {
  for (size_t d = 0; d < docs.size(); ++d) {
    const std::string& doc = docs[d];
    const std::string ref = onePass(doc, styles);
    const bool big = doc.size() > 100000;
    std::vector<MarkerChunkPlan> plans = {{32768, 240}, {4096, 240}, {1000, 3}};
    if (!big) {
      for (uint32_t cb : {1u, 2u, 3u, 5u, 13u, 64u, 257u}) plans.push_back({cb, 240});
      plans.push_back({1, 4});  // cap: the last chunk takes the rest
    }
    for (const auto& plan : plans) {
      for (int coldPct : {0, 35, 100}) {
        const size_t maxRead = (rng() % 3 == 0) ? 0 : 1 + rng() % 500;
        const size_t stageSize = 1 + rng() % 4096;
        const ChunkedResult r = chunked(doc, plan, rng, coldPct, maxRead, stageSize, styles);
        ++cases;
        replays += r.replays;
        skips += r.skips;
        const std::string got = join(r.segments);
        const bool pass = r.ok && got == ref &&
                          static_cast<int>(r.segments.size()) == plan.count(static_cast<uint32_t>(doc.size()));
        if (!pass) {
          if (++failures <= 5) {
            size_t i = 0;
            while (i < got.size() && i < ref.size() && got[i] == ref[i]) ++i;
            std::fprintf(stderr, "FAIL css=%d doc=%zu size=%zu chunk=%u max=%d cold=%d ok=%d segs=%zu firstDiff=%zu "
                                 "ref=%zu got=%zu\n",
                         styles != nullptr ? 1 : 0, d, doc.size(), plan.chunkBytes, plan.maxChunks, coldPct,
                         r.ok ? 1 : 0, r.segments.size(), i, ref.size(), got.size());
          }
        }
      }
    }
  }
  }
  std::fprintf(stderr, "cases=%d replays=%d skips=%d failures=%d cssChangedDocs=%d\n", cases, replays, skips,
               failures, cssChanged);
  runner.expectTrue(failures == 0, "chunked_markers_identical_to_one_pass");
  runner.expectTrue(replays > 0 && skips > 0, "both_replay_and_skip_exercised");

  // A failed write poisons the instance (it consumed bytes whose markers were
  // not stored); a replayed fresh instance then continues identically.
  {
    size_t pick = 0;
    while (pick + 1 < docs.size() && (docs[pick].size() < 3000 || docs[pick].size() > 100000)) ++pick;
    const std::string& doc = docs[pick];
    const std::string ref = onePass(doc, &css);
    const MarkerChunkPlan plan{257, 240};
    const uint32_t size = static_cast<uint32_t>(doc.size());
    std::string out;
    std::vector<uint8_t> buf(300), stage(64);
    ChunkedMarkerizer m(HtmlStripper::Mode::Html, &css);
    std::mt19937 r2(3);
    MarkerizeReadFn src = makeSource(doc, r2, 0);
    const MarkerizeWriteFn okWrite = [&out](const uint8_t* d, size_t n) {
      out.append(reinterpret_cast<const char*>(d), n);
      return true;
    };
    const MarkerizeWriteFn badWrite = [](const uint8_t*, size_t) { return false; };
    bool ok = m.markerizeChunk(src, okWrite, plan.end(0, size), false, buf.data(), buf.size(), stage.data(),
                               stage.size()) == MarkerizeStatus::Success;
    ok = ok && m.markerizeChunk(src, badWrite, plan.end(1, size), false, buf.data(), buf.size(), stage.data(),
                                stage.size()) == MarkerizeStatus::WriteError;
    ok = ok && !m.clean();
    // Recovery: fresh instance, replay chunk 0's range, redo chunks 1..N.
    ChunkedMarkerizer fresh(HtmlStripper::Mode::Html, &css);
    MarkerizeReadFn src2 = makeSource(doc, r2, 0);
    ok = ok && fresh.replayTo(src2, plan.start(1, size), buf.data(), buf.size());
    const int total = plan.count(size);
    for (int k = 1; ok && k < total; ++k) {
      ok = fresh.markerizeChunk(src2, okWrite, plan.end(k, size), k == total - 1, buf.data(), buf.size(),
                                stage.data(), stage.size()) == MarkerizeStatus::Success;
    }
    runner.expectTrue(ok && out == ref, "write_failure_then_replay_recovers");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
