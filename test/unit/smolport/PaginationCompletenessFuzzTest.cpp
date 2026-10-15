#include "test_utils.h"

#include <HtmlStripper.h>
#include <MarkerizeChapter.h>
#include <MarkerizedPageRender.h>
#include <StreamingPaginator.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

// =============================================================================
// PaginationCompletenessFuzzTest — property-based proof that pagination never
// drops or duplicates content.
//
// ORACLE: render the whole chapter onto ONE effectively infinite page.  That
// single continuous walk is the ground truth of "what text exists" — it already
// includes stripper-generated text (list bullets), style-split words, images,
// everything — with identical line widths, so only PAGE breaks differ.  Every
// device-shaped paginated path must then draw exactly the same content stream.
//
// The comparison is CHARACTER-level (all drawn token bytes concatenated, images
// as explicit tokens): token boundaries can legitimately differ when a very
// long word straddles a read-chunk boundary (it is glued from two slots), but
// any lost or duplicated byte, word, sentence, paragraph or image fails.
//
// Paths checked per random chapter (random read-chunk sizes everywhere, so
// partial words / markers split across reads at arbitrary positions):
//   A  device: continuous draw-suppressed walk builds the .idx (EpubChapterParser
//      R4.c), then page 0 + every page N is rendered by seek-resume from its
//      snapshot (ReaderState).  Concatenated pages must equal the oracle.
//   B  sequential reading chain: each page resumes from the boundary captured
//      while drawing the previous page.
//   C  cold skip-to-target (render target=N from offset 0) must draw exactly what
//      path A drew for page N.
//   BND resume-captured next-page boundary must equal the idx walk's boundary
//      for that page (the divergence that causes seams to drop/duplicate).
//   D  v3.11.13 progressive idx: the marker stream grows in random byte
//      prefixes (cut mid-word / mid-marker / mid-UTF-8, as chunk segments end);
//      each step resumes from the last boundary and walks the prefix without the
//      end-of-stream flush (streamComplete=false) until the stream is complete.
//      Boundaries AND anchor pages must equal the continuous walk's.
//
// Usage: PaginationCompletenessFuzzTest [chaptersPerConfig] [verbose=0|1]
// =============================================================================

using namespace snapix::smolport;

namespace {

// ---------------------------------------------------------------------------
// Deterministic renderer: records every drawn token, in order.
// ---------------------------------------------------------------------------
struct RecordingRenderer : public PaginatorRenderer {
  std::vector<std::string> toks;
  uint16_t measureWidth(const uint8_t* /*t*/, size_t len, uint8_t sb) override {
    unsigned per = 6;
    if (sb & (kStyleSuper | kStyleSub)) {
      per = 4;
    } else if (sb & kStyleHeading) {
      per = 8;
    } else if (sb & kStyleBold) {
      per = 7;
    }
    const size_t w = len * per;
    return static_cast<uint16_t>(w > 65535 ? 65535 : w);
  }
  uint16_t getSpaceWidth(uint8_t sb) override { return (sb & kStyleHeading) ? 5 : 4; }
  void drawWord(uint16_t, uint16_t, const uint8_t* t, size_t len, uint8_t) override {
    toks.emplace_back(reinterpret_cast<const char*>(t), len);
  }
  // Image path format: "img<W>_<H>_<N>".
  bool resolveImageSize(const uint8_t* p, size_t len, uint16_t* w, uint16_t* h) override {
    const std::string s(reinterpret_cast<const char*>(p), len);
    unsigned W = 0, H = 0, N = 0;
    if (std::sscanf(s.c_str(), "img%u_%u_%u", &W, &H, &N) != 3 || W == 0 || H == 0) return false;
    *w = static_cast<uint16_t>(W);
    *h = static_cast<uint16_t>(H);
    return true;
  }
  void drawImage(uint16_t, uint16_t, uint16_t, uint16_t, const uint8_t* p, size_t len) override {
    toks.push_back("\x02IMG:" + std::string(reinterpret_cast<const char*>(p), len) + "\x02");
  }
};

// Reader that serves the marker stream from `start` in RANDOM-sized chunks
// (maxChunk == 0 → as large as the caller's buffer allows).
struct ChunkReaderState {
  const std::vector<uint8_t>* data = nullptr;
  size_t pos = 0;
  std::mt19937 rng;
  size_t maxChunk = 0;
};

ChapterReadFn makeReader(const std::vector<uint8_t>& data, size_t start, uint32_t seed, size_t maxChunk) {
  auto st = std::make_shared<ChunkReaderState>();
  st->data = &data;
  st->pos = start;
  st->rng.seed(seed);
  st->maxChunk = maxChunk;
  return [st](uint8_t* buf, size_t bufSize) -> int {
    if (st->pos >= st->data->size()) return 0;
    size_t n = std::min(bufSize, st->data->size() - st->pos);
    if (st->maxChunk > 0) {
      const size_t r = 1 + st->rng() % st->maxChunk;
      n = std::min(n, r);
    }
    std::memcpy(buf, st->data->data() + st->pos, n);
    st->pos += n;
    return static_cast<int>(n);
  };
}

std::vector<uint8_t> markerize(const std::string& html) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> in(html.begin(), html.end());
  size_t pos = 0;
  uint8_t buf[256];
  markerizeChapter(
      HtmlStripper::Mode::Html,
      [&](uint8_t* b, size_t n) -> int {
        if (pos >= in.size()) return 0;
        const size_t k = std::min(n, in.size() - pos);
        std::memcpy(b, in.data() + pos, k);
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

struct PageRender {
  std::vector<std::string> toks;
  std::vector<PageBoundarySnapshot> caps;
  MarkerizedRenderResult status = MarkerizedRenderResult::ReadError;
};

PageRender renderPage(const StreamingPaginatorConfig& cfg, const std::vector<uint8_t>& m, uint16_t target,
                      const MarkerizedRenderResume& resume, uint32_t seed, size_t maxChunk) {
  RecordingRenderer rr;
  StreamingPaginator pag(cfg, rr);
  uint8_t buf[256];
  PageRender out;
  const size_t start = resume.startPage > 0 ? resume.byteOffset : 0;
  out.status = renderMarkerizedPage(pag, makeReader(m, start, seed, maxChunk), buf, sizeof(buf), target, {},
                                    nullptr, resume,
                                    [&](const PageBoundarySnapshot& s) { out.caps.push_back(s); });
  out.toks = std::move(rr.toks);
  return out;
}

MarkerizedRenderResume fromSnap(const PageBoundarySnapshot& s) {
  MarkerizedRenderResume r;
  r.startPage = s.pageIndex;
  r.byteOffset = s.byteOffset;
  r.styleBits = s.styleBits;
  r.atParagraphStart = s.atParagraphStart;
  r.indentDepth = s.indentDepth;
  r.centered = s.centered;
  return r;
}

std::string joinChars(const std::vector<std::string>& toks, bool stripHyphens) {
  std::string s;
  for (const auto& t : toks) {
    for (char c : t) {
      if (stripHyphens && c == '-') continue;
      s.push_back(c);
    }
  }
  return s;
}

// ---------------------------------------------------------------------------
// Random chapter generator.
// ---------------------------------------------------------------------------
struct GenOptions {
  bool allowCyrillic = true;
  bool allowLongWords = true;      // 20..90 bytes (some exceed the 64-byte carry)
  bool allowHugeWords = true;      // > line-text capacity (256 bytes)
  bool allowImages = true;         // images that fit an empty page
  bool allowTallImages = true;     // images taller than the content area
  bool allowNbsp = true;
  bool allowStyles = true;         // <b>/<i>/<sup>, style toggles inside a word
  bool allowAnchors = true;        // <a id> between words
  bool allowBr = true;             // <br/> inside running text
  bool allowBlocks = true;         // quotes, lists, tables, center, dl, …
  bool asciiNoHyphen = false;      // hyphenation oracle: ASCII words, no '-'
  uint16_t imgMaxW = 100;
  uint16_t imgMaxH = 80;
};

struct Gen {
  std::mt19937 rng;
  GenOptions opt;
  int imgSeq = 0;

  int rnd(int lo, int hi) { return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)); }
  bool chance(int pct) { return rnd(1, 100) <= pct; }

  std::string asciiWord(int len) {
    std::string w;
    for (int i = 0; i < len; ++i) w.push_back(static_cast<char>('a' + rnd(0, 25)));
    return w;
  }
  std::string cyrWord(int len) {
    std::string w;
    for (int i = 0; i < len; ++i) {
      // U+0430..U+044F (а..я) → D0 B0..D0 BF, D1 80..D1 8F
      const int cp = 0x430 + rnd(0, 31);
      w.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      w.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return w;
  }
  std::string word() {
    if (opt.asciiNoHyphen) return asciiWord(rnd(1, 14));
    const int r = rnd(1, 100);
    if (opt.allowHugeWords && r <= 1) return asciiWord(rnd(260, 320));
    if (opt.allowLongWords && r <= 4) return asciiWord(rnd(20, 90));
    if (opt.allowCyrillic && r <= 22) return cyrWord(rnd(1, 14));
    return asciiWord(rnd(1, 10));
  }

  // One "item" inside running text.
  std::string textItem() {
    if (opt.asciiNoHyphen) return word();
    const int r = rnd(1, 100);
    if (opt.allowStyles && r <= 4) return "<b>" + word() + " " + word() + "</b>";
    if (opt.allowStyles && r <= 7) return "<i>" + word() + "</i>";
    if (opt.allowStyles && r <= 9) return word() + "<sup>" + std::to_string(rnd(1, 99)) + "</sup>";
    if (opt.allowStyles && r <= 11) {  // style toggle INSIDE a word: хо<b>ро</b>шо
      return asciiWord(rnd(1, 4)) + "<b>" + asciiWord(rnd(1, 4)) + "</b>" + asciiWord(rnd(1, 4));
    }
    if (opt.allowAnchors && r <= 13) return "<a id=\"a" + std::to_string(rnd(1, 999)) + "\"></a>" + word();
    // v3.11.12 — anchor INSIDE a word (что<a id/>написать, word<span id>part):
    // the glued halves must keep exact source offsets when a long word is
    // split / carried across a page boundary.
    if (opt.allowAnchors && r <= 15) {
      return word() + "<a id=\"m" + std::to_string(rnd(1, 999)) + "\"></a>" + word() +
             (chance(30) ? "<span id=\"s" + std::to_string(rnd(1, 999)) + "\">" + word() + "</span>" : "");
    }
    if (opt.allowNbsp && r <= 19) return word() + "&nbsp;" + word() + "&nbsp;" + word();
    if (opt.allowBr && r <= 20) return word() + "<br/>" + word();
    return word();
  }

  std::string run(int minItems, int maxItems) {
    std::string s;
    const int n = rnd(minItems, maxItems);
    for (int i = 0; i < n; ++i) {
      if (i) s += " ";
      s += textItem();
    }
    return s;
  }

  std::string image() {
    uint16_t h = static_cast<uint16_t>(rnd(4, opt.imgMaxH));
    if (opt.allowTallImages && chance(30)) h = static_cast<uint16_t>(opt.imgMaxH + rnd(1, opt.imgMaxH / 2));
    const uint16_t w = static_cast<uint16_t>(rnd(4, opt.imgMaxW));
    return "<img src=\"img" + std::to_string(w) + "_" + std::to_string(h) + "_" + std::to_string(++imgSeq) +
           "\"/>";
  }

  std::string block(int depth) {
    const int r = rnd(1, 100);
    if (opt.asciiNoHyphen || !opt.allowBlocks) {
      if (r <= 15) return "<h2>" + run(1, 4) + "</h2>";
      if (!opt.asciiNoHyphen && (opt.allowImages || opt.allowTallImages) && r <= 25) return image();
      return "<p>" + run(3, 40) + "</p>";
    }
    if (r <= 40) return "<p>" + run(1, 45) + "</p>";
    if (r <= 47) return "<h" + std::to_string(rnd(1, 3)) + ">" + run(1, 6) + "</h" + std::to_string(rnd(1, 3)) + ">";
    if (r <= 55 && depth < 2) {
      std::string s = "<blockquote>";
      const int n = rnd(1, 4);
      for (int i = 0; i < n; ++i) s += block(depth + 1);
      return s + "</blockquote>";
    }
    if (r <= 62 && depth < 2) {
      std::string s = chance(50) ? "<ul>" : "<ol>";
      const int n = rnd(1, 6);
      for (int i = 0; i < n; ++i) s += "<li>" + run(1, 12) + "</li>";
      return s + "</ul>";
    }
    if (r <= 70 && (opt.allowImages || opt.allowTallImages)) return image();
    if (r <= 73) return "<hr/>";
    if (r <= 78) return "<div>" + run(1, 20) + "</div>";
    if (r <= 82) {
      std::string s = "<table>";
      const int rows = rnd(1, 3);
      for (int i = 0; i < rows; ++i) {
        s += "<tr>";
        const int cols = rnd(1, 3);
        for (int c = 0; c < cols; ++c) s += "<td>" + run(1, 3) + "</td>";
        s += "</tr>";
      }
      return s + "</table>";
    }
    if (r <= 85) return "<center>" + run(1, 8) + "</center>";
    if (r <= 88) return "<section>" + run(1, 10) + "</section>";
    if (r <= 91) return "<dl><dt>" + run(1, 3) + "</dt><dd>" + run(1, 10) + "</dd></dl>";
    if (r <= 94) return "<p>" + run(1, 30);  // unclosed <p>: text runs into the next block
    return "<p>" + run(10, 80) + "</p>";     // long paragraph → many carries/spillover
  }

  std::string chapter() {
    std::string s = "<html><head><style>p{}</style></head><body>";
    const int n = rnd(2, 22);
    for (int i = 0; i < n; ++i) s += block(0);
    return s + "</body></html>";
  }
};

// ---------------------------------------------------------------------------
// Failure accounting.
// ---------------------------------------------------------------------------
struct Category {
  const char* name;
  int failures = 0;
  int printed = 0;
};

bool g_verbose = false;

void describeDiff(const char* what, const std::vector<std::string>& ref, const std::vector<std::string>& got,
                  bool stripHyphens) {
  const std::string a = joinChars(ref, stripHyphens);
  const std::string b = joinChars(got, stripHyphens);
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
  auto ctx = [](const std::string& s, size_t at) {
    const size_t from = at > 40 ? at - 40 : 0;
    std::string out = s.substr(from, 80);
    for (char& c : out) {
      if (c == '\x02') c = '|';
    }
    return out;
  };
  std::fprintf(stderr, "    %s: refLen=%zu gotLen=%zu firstDiffAt=%zu (%s)\n", what, a.size(), b.size(), i,
               b.size() < a.size() ? "MISSING content" : (b.size() > a.size() ? "EXTRA content" : "REORDERED"));
  std::fprintf(stderr, "      ref: ...%s...\n", ctx(a, i).c_str());
  std::fprintf(stderr, "      got: ...%s...\n", ctx(b, i).c_str());
}

bool sameSnap(const PageBoundarySnapshot& x, const PageBoundarySnapshot& y) {
  return x.pageIndex == y.pageIndex && x.byteOffset == y.byteOffset && x.styleBits == y.styleBits &&
         x.atParagraphStart == y.atParagraphStart && x.indentDepth == y.indentDepth && x.centered == y.centered;
}

struct RunResult {
  bool skipped = false;
  bool okA = true, okB = true, okC = true, okBnd = true, okD = true;
};

using AnchorList = std::vector<std::pair<std::string, uint16_t>>;

// Continuous walk capturing boundaries + anchor pages (EpubChapterParser R4.c).
void continuousWalk(const StreamingPaginatorConfig& cfg, const std::vector<uint8_t>& m, uint32_t seed,
                    std::vector<PageBoundarySnapshot>& caps, AnchorList& anchors) {
  RecordingRenderer rr;
  StreamingPaginator pag(cfg, rr);
  uint8_t buf[256];
  renderMarkerizedPage(
      pag, makeReader(m, 0, seed, 97), buf, sizeof(buf), UINT16_MAX, {}, nullptr, {},
      [&](const PageBoundarySnapshot& s) { caps.push_back(s); },
      [&](const uint8_t* id, uint16_t len, uint16_t page) {
        anchors.emplace_back(std::string(reinterpret_cast<const char*>(id), len), page);
      });
}

// Progressive walk over growing prefixes (EpubChapterParser::extendProgressiveIdx).
void progressiveWalk(const StreamingPaginatorConfig& cfg, const std::vector<uint8_t>& m, uint32_t seed,
                     std::vector<PageBoundarySnapshot>& caps, AnchorList& anchors) {
  std::mt19937 rng(seed);
  // Dense cuts (a stream grown a few dozen bytes at a time) so prefix ends
  // regularly land on the last word of a full page.
  std::vector<size_t> ends;
  if (m.size() > 1) {
    const size_t step = 16 + rng() % 400;
    for (size_t at = 1 + rng() % step; at < m.size(); at += 1 + rng() % step) ends.push_back(at);
  }
  ends.push_back(m.size());
  for (size_t step = 0; step < ends.size(); ++step) {
    const bool complete = ends[step] == m.size();
    const std::vector<uint8_t> prefix(m.begin(), m.begin() + static_cast<std::ptrdiff_t>(ends[step]));
    MarkerizedRenderResume resume;
    if (!caps.empty()) resume = resumeFromBoundary(caps.back());
    const uint16_t resumePage = caps.empty() ? 0 : caps.back().pageIndex;
    anchors.erase(std::remove_if(anchors.begin(), anchors.end(),
                                 [resumePage](const AnchorList::value_type& a) { return a.second >= resumePage; }),
                  anchors.end());
    RecordingRenderer rr;
    StreamingPaginator pag(cfg, rr);
    uint8_t buf[256];
    std::vector<PageBoundarySnapshot> fresh;
    renderMarkerizedPage(
        pag, makeReader(prefix, caps.empty() ? 0 : caps.back().byteOffset, seed ^ static_cast<uint32_t>(step), 89),
        buf, sizeof(buf), UINT16_MAX, {}, nullptr, resume, [&](const PageBoundarySnapshot& s) { fresh.push_back(s); },
        [&](const uint8_t* id, uint16_t len, uint16_t page) {
          anchors.emplace_back(std::string(reinterpret_cast<const char*>(id), len), page);
        },
        complete);
    caps.insert(caps.end(), fresh.begin(), fresh.end());
  }
}

RunResult runOne(const StreamingPaginatorConfig& cfg, const std::string& html, uint32_t seed, bool hyphen,
                 Category* cats, const char* cfgName) {
  RunResult res;
  const auto m = markerize(html);

  // ORACLE: one tall page.
  StreamingPaginatorConfig tall = cfg;
  tall.pageHeight = 60000;
  const PageRender ref = renderPage(tall, m, 0, {}, seed ^ 0x51u, 0);
  if (!ref.caps.empty()) {  // chapter too long for the oracle page — skip
    res.skipped = true;
    return res;
  }

  // ---- Path A: device idx + per-page seek-resume. ----
  const PageRender walk = renderPage(cfg, m, UINT16_MAX, {}, seed ^ 0xA1u, 97);
  const auto& snaps = walk.caps;
  std::vector<std::vector<std::string>> pagesA;
  std::vector<PageRender> rendersA;
  rendersA.push_back(renderPage(cfg, m, 0, {}, seed ^ 0xB2u, 61));
  for (size_t k = 0; k < snaps.size(); ++k) {
    rendersA.push_back(renderPage(cfg, m, snaps[k].pageIndex, fromSnap(snaps[k]), seed ^ (0xC3u + k), 113));
  }
  std::vector<std::string> flatA;
  for (const auto& pr : rendersA) {
    pagesA.push_back(pr.toks);
    flatA.insert(flatA.end(), pr.toks.begin(), pr.toks.end());
  }
  if (joinChars(flatA, hyphen) != joinChars(ref.toks, hyphen)) {
    res.okA = false;
    Category& c = cats[0];
    if (++c.failures, c.printed < 4 || g_verbose) {
      ++c.printed;
      std::fprintf(stderr, "[A] %s seed=%u pages=%zu\n", cfgName, seed, rendersA.size());
      describeDiff("A", ref.toks, flatA, hyphen);
    }
  }

  // ---- BND: resume-captured next boundary == idx boundary. ----
  for (size_t p = 0; p < rendersA.size() && res.okBnd; ++p) {
    const auto& caps = rendersA[p].caps;
    if (p < snaps.size()) {
      if (caps.empty() || !sameSnap(caps.front(), snaps[p])) {
        res.okBnd = false;
        Category& c = cats[3];
        if (++c.failures, c.printed < 4 || g_verbose) {
          ++c.printed;
          std::fprintf(stderr, "[BND] %s seed=%u page=%zu idx(off=%u ind=%u para=%d st=%u) resume(", cfgName, seed,
                       p, snaps[p].byteOffset, snaps[p].indentDepth, snaps[p].atParagraphStart ? 1 : 0,
                       snaps[p].styleBits);
          if (caps.empty()) {
            std::fprintf(stderr, "none)\n");
          } else {
            std::fprintf(stderr, "off=%u ind=%u para=%d st=%u)\n", caps.front().byteOffset,
                         caps.front().indentDepth, caps.front().atParagraphStart ? 1 : 0, caps.front().styleBits);
          }
        }
      }
    }
  }

  // ---- Path B: sequential reading chain. ----
  {
    std::vector<std::string> flatB;
    MarkerizedRenderResume r;
    uint16_t page = 0;
    for (int guard = 0; guard < 5000; ++guard) {
      const PageRender pr = renderPage(cfg, m, page, r, seed ^ (0xD4u + guard), 89);
      flatB.insert(flatB.end(), pr.toks.begin(), pr.toks.end());
      if (pr.caps.empty()) break;
      r = fromSnap(pr.caps.front());
      page = pr.caps.front().pageIndex;
    }
    if (joinChars(flatB, hyphen) != joinChars(ref.toks, hyphen)) {
      res.okB = false;
      Category& c = cats[1];
      if (++c.failures, c.printed < 4 || g_verbose) {
        ++c.printed;
        std::fprintf(stderr, "[B] %s seed=%u\n", cfgName, seed);
        describeDiff("B", ref.toks, flatB, hyphen);
      }
    }
  }

  // ---- Path D: progressive idx over growing prefixes == continuous walk. ----
  {
    std::vector<PageBoundarySnapshot> refCaps, gotCaps;
    AnchorList refAnchors, gotAnchors;
    continuousWalk(cfg, m, seed ^ 0xF6u, refCaps, refAnchors);
    progressiveWalk(cfg, m, seed ^ 0x17u, gotCaps, gotAnchors);
    bool same = refCaps.size() == gotCaps.size() && refAnchors == gotAnchors;
    size_t firstBad = 0;
    while (same && firstBad < refCaps.size() && sameSnap(refCaps[firstBad], gotCaps[firstBad])) ++firstBad;
    if (same && firstBad < refCaps.size()) same = false;
    if (!same) {
      res.okD = false;
      Category& c = cats[4];
      if (++c.failures, c.printed < 4 || g_verbose) {
        ++c.printed;
        std::fprintf(stderr, "[D] %s seed=%u boundaries ref=%zu got=%zu anchors ref=%zu got=%zu firstBad=%zu\n",
                     cfgName, seed, refCaps.size(), gotCaps.size(), refAnchors.size(), gotAnchors.size(),
                     firstBad);
      }
    }
  }

  // ---- Path C: cold skip-to-target == path A per page. ----
  for (size_t p = 1; p < pagesA.size() && res.okC; ++p) {
    const PageRender pr = renderPage(cfg, m, static_cast<uint16_t>(p), {}, seed ^ (0xE5u + p), 71);
    if (joinChars(pr.toks, hyphen) != joinChars(pagesA[p], hyphen)) {
      res.okC = false;
      Category& c = cats[2];
      if (++c.failures, c.printed < 4 || g_verbose) {
        ++c.printed;
        std::fprintf(stderr, "[C] %s seed=%u page=%zu\n", cfgName, seed, p);
        describeDiff("C", pagesA[p], pr.toks, hyphen);
      }
    }
  }
  return res;
}

// Render marker bytes [from, to) readably: text as-is, markers as <tag>.
std::string showMarkers(const std::vector<uint8_t>& m, size_t from, size_t to) {
  std::string s;
  to = std::min(to, m.size());
  for (size_t i = from; i < to; ++i) {
    if (m[i] == 0x01 && i + 1 < to) {
      s += "<";
      s.push_back(static_cast<char>(m[i + 1]));
      s += ">";
      ++i;
    } else if (m[i] >= 0x20 || m[i] >= 0x80) {
      s.push_back(static_cast<char>(m[i]));
    } else {
      s += "\\x";
      static const char* hex = "0123456789abcdef";
      s.push_back(hex[m[i] >> 4]);
      s.push_back(hex[m[i] & 15]);
    }
  }
  return s;
}

std::string joinToks(const std::vector<std::string>& toks) {
  std::string s;
  for (const auto& t : toks) {
    if (!s.empty()) s += " ";
    for (char c : t) s.push_back(c == '\x02' ? '|' : c);
  }
  return s;
}

void dumpCase(const StreamingPaginatorConfig& cfg, const std::string& html, uint32_t seed) {
  const auto m = markerize(html);
  std::fprintf(stderr, "===== DUMP seed=%u markers=%zu =====\nHTML: %s\n", seed, m.size(), html.c_str());
  const PageRender walk = renderPage(cfg, m, UINT16_MAX, {}, seed ^ 0xA1u, 97);
  std::vector<PageRender> a;
  a.push_back(renderPage(cfg, m, 0, {}, seed ^ 0xB2u, 61));
  for (size_t k = 0; k < walk.caps.size(); ++k) {
    a.push_back(renderPage(cfg, m, walk.caps[k].pageIndex, fromSnap(walk.caps[k]), seed ^ (0xC3u + k), 113));
  }
  for (size_t p = 0; p < a.size(); ++p) {
    if (p > 0) {
      const auto& s = walk.caps[p - 1];
      std::fprintf(stderr, "-- idx[%zu] off=%u ind=%u ctr=%d para=%d st=%u   around: [%s]^[%s]\n", p, s.byteOffset,
                   s.indentDepth, s.centered ? 1 : 0, s.atParagraphStart ? 1 : 0, s.styleBits,
                   showMarkers(m, s.byteOffset > 24 ? s.byteOffset - 24 : 0, s.byteOffset).c_str(),
                   showMarkers(m, s.byteOffset, s.byteOffset + 24).c_str());
    }
    std::fprintf(stderr, "A page %zu: %s\n", p, joinToks(a[p].toks).c_str());
    if (!a[p].caps.empty()) {
      std::fprintf(stderr, "   resume-captured next: off=%u ind=%u para=%d\n", a[p].caps.front().byteOffset,
                   a[p].caps.front().indentDepth, a[p].caps.front().atParagraphStart ? 1 : 0);
    }
    if (p > 0) {
      const PageRender c = renderPage(cfg, m, static_cast<uint16_t>(p), {}, seed ^ (0xE5u + p), 71);
      std::fprintf(stderr, "C page %zu: %s\n", p, joinToks(c.toks).c_str());
    }
  }
}

StreamingPaginatorConfig makeCfg(uint16_t w, uint16_t h, uint16_t indent, bool hyph) {
  StreamingPaginatorConfig c{};
  c.pageWidth = w;
  c.pageHeight = h;
  c.marginTop = 4;
  c.marginBottom = 6;
  c.marginLeft = 5;
  c.marginRight = 5;
  c.bodyLineHeight = 12;
  c.headingLineHeight = 18;
  c.paragraphSpacing = 4;
  c.firstLineIndent = indent;
  c.hyphenate = hyph;
  std::strcpy(c.hyphenLang, hyph ? "en" : "");
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  TestUtils::TestRunner runner("PaginationCompletenessFuzz");
  const int perConfig = argc > 1 ? std::max(1, std::atoi(argv[1])) : 150;
  g_verbose = argc > 2 && std::atoi(argv[2]) != 0;

  struct Scenario {
    const char* name;
    StreamingPaginatorConfig cfg;
    GenOptions opt;
    bool hyphen;
  };
  std::vector<Scenario> scenarios;
  {
    GenOptions o;
    o.imgMaxW = 100;
    o.imgMaxH = 80;
    scenarios.push_back({"narrow", makeCfg(120, 100, 0, false), o, false});
  }
  {
    GenOptions o;
    o.imgMaxW = 100;
    o.imgMaxH = 140;
    scenarios.push_back({"narrow+indent", makeCfg(120, 160, 12, false), o, false});
  }
  {
    // Wide page: words of 65..78 bytes FIT a line but exceed the 64-byte carry.
    GenOptions o;
    o.imgMaxW = 400;
    o.imgMaxH = 110;
    scenarios.push_back({"wide", makeCfg(480, 130, 18, false), o, false});
  }
  {
    GenOptions o;
    o.asciiNoHyphen = true;
    o.allowImages = false;
    scenarios.push_back({"hyphenate", makeCfg(140, 110, 0, true), o, true});
  }

  // Optional profile (argv[3]) isolates one feature class on top of plain text.
  const std::string profile = argc > 3 ? argv[3] : "all";
  if (profile != "all") {
    for (auto& sc : scenarios) {
      if (sc.hyphen) continue;
      GenOptions& o = sc.opt;
      o.allowLongWords = o.allowHugeWords = o.allowImages = o.allowTallImages = false;
      o.allowNbsp = o.allowStyles = o.allowAnchors = o.allowBr = o.allowBlocks = false;
      if (profile == "blocks") o.allowBlocks = true;
      if (profile == "styles") o.allowStyles = o.allowAnchors = o.allowBr = true;
      if (profile == "long") o.allowLongWords = true;
      if (profile == "huge") o.allowHugeWords = true;
      if (profile == "img") o.allowImages = true;
      if (profile == "tall") o.allowTallImages = true;
      if (profile == "nbsp") o.allowNbsp = true;
    }
  }
  std::fprintf(stderr, "profile=%s chaptersPerConfig=%d\n", profile.c_str(), perConfig);

  // FUZZ_DUMP=<seed>: dump html + per-page A/C tokens for that seed.
  const char* dumpEnv = std::getenv("FUZZ_DUMP");
  const uint32_t dumpSeed = dumpEnv ? static_cast<uint32_t>(std::strtoul(dumpEnv, nullptr, 10)) : 0;

  int total = 0, skipped = 0;
  for (const auto& sc : scenarios) {
    Category cats[5] = {{"A device idx+resume"}, {"B sequential chain"}, {"C skip-to-target"},
                        {"BND boundary agreement"}, {"D progressive idx"}};
    int ran = 0;
    for (int i = 0; i < perConfig; ++i) {
      const uint32_t seed = 0x9E3779B9u * static_cast<uint32_t>(i + 1) + static_cast<uint32_t>(sc.cfg.pageWidth);
      Gen g;
      g.rng.seed(seed);
      g.opt = sc.opt;
      const std::string html = g.chapter();
      if (dumpSeed != 0 && seed == dumpSeed) dumpCase(sc.cfg, html, seed);
      const RunResult rr = runOne(sc.cfg, html, seed, sc.hyphen, cats, sc.name);
      ++total;
      if (rr.skipped) {
        ++skipped;
        continue;
      }
      ++ran;
    }
    std::fprintf(stderr, "== %-14s chapters=%d  A=%d  B=%d  C=%d  BND=%d  D=%d failures\n", sc.name, ran,
                 cats[0].failures, cats[1].failures, cats[2].failures, cats[3].failures, cats[4].failures);
    for (const auto& c : cats) {
      runner.expectTrue(c.failures == 0, (std::string(sc.name) + ": " + c.name).c_str());
    }
  }
  std::fprintf(stderr, "total=%d skipped(too long for oracle)=%d\n", total, skipped);
  runner.expectTrue(skipped * 10 < total, "oracle_covers_most_chapters");

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
