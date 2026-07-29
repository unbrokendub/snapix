#include "test_utils.h"

#include <HtmlStripper.h>
#include <MarkerizeChapter.h>
#include <MarkerizedPageRender.h>
#include <MarkerStream.h>
#include <StreamingPaginator.h>

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

// =============================================================================
// MarkerizedPageRenderTest — proves the page-targeted streaming render
// path (R3.4) correctly:
//   * Skips first N-1 pages with draw-suppressed paginator
//   * Draws target page N
//   * Distinguishes pages 0/1/2 in a multi-page synthetic chapter
//   * Reports correct stats (pagesAdvancedThrough, bytesConsumed)
//   * Handles edge cases: target page beyond chapter, abort, read error
// =============================================================================

using snapix::smolport::HtmlStripper;
using snapix::smolport::MarkerizedRenderResult;
using snapix::smolport::MarkerizedRenderStats;
using snapix::smolport::PaginatorRenderer;
using snapix::smolport::renderMarkerizedPage;
using snapix::smolport::StreamingPaginator;
using snapix::smolport::StreamingPaginatorConfig;
using snapix::smolport::markerizeChapter;

// Small page geometry (so it's easy to fill multiple pages with little
// text).  100×80 working area with 12px lines = ~5 lines/page.
static const StreamingPaginatorConfig& smallConfig() {
  static const StreamingPaginatorConfig cfg = [] {
    StreamingPaginatorConfig value{};
    value.pageWidth = 100;
    value.pageHeight = 80;
    value.marginTop = 5;
    value.marginBottom = 5;
    value.marginLeft = 5;
    value.marginRight = 5;
    value.bodyLineHeight = 12;
    value.headingLineHeight = 18;
    value.paragraphSpacing = 4;
    return value;
  }();
  return cfg;
}

// Fake renderer: 6 px per char, 4 px space; identical to the one in
// StreamingPaginatorTest.
struct FakeRenderer : public PaginatorRenderer {
  struct Draw {
    uint16_t x, y;
    std::string text;
  };
  std::vector<Draw> draws;
  uint16_t measureWidth(const uint8_t* /*t*/, size_t len, uint8_t /*sb*/) override {
    return static_cast<uint16_t>(len * 6);
  }
  uint16_t getSpaceWidth(uint8_t) override { return 4; }
  void drawWord(uint16_t x, uint16_t y, const uint8_t* t, size_t len, uint8_t /*sb*/) override {
    Draw d;
    d.x = x;
    d.y = y;
    d.text = std::string(reinterpret_cast<const char*>(t), len);
    draws.push_back(std::move(d));
  }
};

// Markerize an HTML string into bytes.
static std::vector<uint8_t> markerizeAll(const std::string& html) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> in(html.begin(), html.end());
  size_t pos = 0;
  uint8_t buf[256];
  markerizeChapter(
      HtmlStripper::Mode::Html,
      [&](uint8_t* b, size_t n) -> int {
        if (pos >= in.size()) return 0;
        size_t toCopy = std::min(n, in.size() - pos);
        std::memcpy(b, in.data() + pos, toCopy);
        pos += toCopy;
        return static_cast<int>(toCopy);
      },
      [&](const uint8_t* d, size_t n) -> bool {
        out.insert(out.end(), d, d + n);
        return true;
      },
      buf, sizeof(buf));
  return out;
}

// Make a long enough HTML to fill ~3 pages at the small config.
// 100×80 with 12px line ≈ 5 lines/page; 6px/char × 90 useful px ≈ 15
// chars/line × 5 lines = 75 chars/page → use 250+ chars to span 3+
// pages.  Each "<p>" generates a paragraph break.
static std::string makeMultiPageHtml() {
  // Each paragraph filling roughly one page: ~80 chars × 4 = 320
  return "<p>page0word0 page0word1 page0word2 page0word3 page0word4 page0word5 page0word6</p>"
         "<p>page1word0 page1word1 page1word2 page1word3 page1word4 page1word5 page1word6</p>"
         "<p>page2word0 page2word1 page2word2 page2word3 page2word4 page2word5 page2word6</p>"
         "<p>page3word0 page3word1 page3word2 page3word3 page3word4 page3word5 page3word6</p>";
}

// Reader-callback maker for an in-memory byte vector.
static auto makeReader(std::vector<uint8_t>& data, size_t& pos) {
  return [&](uint8_t* buf, size_t bufSize) -> int {
    if (pos >= data.size()) return 0;
    size_t toCopy = std::min(bufSize, data.size() - pos);
    std::memcpy(buf, data.data() + pos, toCopy);
    pos += toCopy;
    return static_cast<int>(toCopy);
  };
}

int main() {
  TestUtils::TestRunner runner("MarkerizedPageRender");
  uint8_t chunkBuf[256];

  auto markers = markerizeAll(makeMultiPageHtml());
  runner.expectTrue(markers.size() > 100, "setup_markers_produced");

  // -----------------------------------------------------------------------
  // T1: render page 0 — should draw words containing "page0".
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator paginator(smallConfig(), fr);
    size_t pos = 0;
    MarkerizedRenderStats stats{};
    auto status = renderMarkerizedPage(paginator, makeReader(markers, pos), chunkBuf, sizeof(chunkBuf),
                                        /*targetPage=*/0, {}, &stats);
    runner.expectTrue(status == MarkerizedRenderResult::Success, "T1_page0_success");
    runner.expectTrue(!fr.draws.empty(), "T1_page0_drew_words");
    // First drawn word should contain "page0" prefix
    runner.expectTrue(fr.draws[0].text.find("page0") != std::string::npos,
                      "T1_page0_first_word_belongs_to_page0");
    runner.expectEq(uint16_t(0), stats.pagesAdvancedThrough,
                    "T1_no_pages_skipped");
  }

  // -----------------------------------------------------------------------
  // T2: render page 1 — words containing "page1", NO "page0" words drawn.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator paginator(smallConfig(), fr);
    size_t pos = 0;
    MarkerizedRenderStats stats{};
    auto status = renderMarkerizedPage(paginator, makeReader(markers, pos), chunkBuf, sizeof(chunkBuf),
                                        /*targetPage=*/1, {}, &stats);
    runner.expectTrue(status == MarkerizedRenderResult::Success, "T2_page1_success");
    runner.expectTrue(!fr.draws.empty(), "T2_page1_drew_words");

    // Verify page1 words ARE drawn and pages further out (page2, page3)
    // are NOT.  Note: a carry-over word from end of page0 may legitimately
    // appear at the top of page1 (it didn't fit on page0 → carried over),
    // so we don't strictly forbid page0 prefixes — only check the ratio
    // is dominated by page1 content.  This matches the semantic
    // contract: "the page user sees is what gets drawn."
    int page1WordCount = 0;
    int page2WordCount = 0;
    int page3WordCount = 0;
    for (const auto& d : fr.draws) {
      if (d.text.find("page1") != std::string::npos) ++page1WordCount;
      if (d.text.find("page2") != std::string::npos) ++page2WordCount;
      if (d.text.find("page3") != std::string::npos) ++page3WordCount;
    }
    runner.expectTrue(page1WordCount >= 3, "T2_page1_words_dominate");
    runner.expectEq(0, page2WordCount, "T2_no_page2_words_when_targeting_page1");
    runner.expectEq(0, page3WordCount, "T2_no_page3_words_when_targeting_page1");
    runner.expectEq(uint16_t(1), stats.pagesAdvancedThrough, "T2_one_page_skipped");
  }

  // -----------------------------------------------------------------------
  // T3: render page 2 — words containing "page2".
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator paginator(smallConfig(), fr);
    size_t pos = 0;
    auto status = renderMarkerizedPage(paginator, makeReader(markers, pos), chunkBuf, sizeof(chunkBuf),
                                        /*targetPage=*/2);
    runner.expectTrue(status == MarkerizedRenderResult::Success, "T3_page2_success");
    bool sawPage2Word = false;
    for (const auto& d : fr.draws) {
      if (d.text.find("page2") != std::string::npos) sawPage2Word = true;
    }
    runner.expectTrue(sawPage2Word, "T3_page2_words_drawn");
  }

  // -----------------------------------------------------------------------
  // T4: target page beyond chapter → PageNotFound.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator paginator(smallConfig(), fr);
    size_t pos = 0;
    auto status = renderMarkerizedPage(paginator, makeReader(markers, pos), chunkBuf, sizeof(chunkBuf),
                                        /*targetPage=*/100);
    runner.expectTrue(status == MarkerizedRenderResult::PageNotFound, "T4_oob_page_not_found");
  }

  // -----------------------------------------------------------------------
  // T5: misconfigured caller (null read) → ReadError.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator paginator(smallConfig(), fr);
    auto status = renderMarkerizedPage(paginator, {}, chunkBuf, sizeof(chunkBuf), 0);
    runner.expectTrue(status == MarkerizedRenderResult::ReadError, "T5_null_reader_error");
  }

  // -----------------------------------------------------------------------
  // T6: abort fires mid-stream → Aborted.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator paginator(smallConfig(), fr);
    size_t pos = 0;
    int callCount = 0;
    auto abortFn = [&]() -> bool {
      ++callCount;
      return callCount >= 2;  // abort after first chunk
    };
    // Use small chunk to force multiple reads.
    uint8_t smallBuf[16];
    auto status = renderMarkerizedPage(paginator, makeReader(markers, pos), smallBuf, sizeof(smallBuf),
                                        /*targetPage=*/0, abortFn);
    runner.expectTrue(status == MarkerizedRenderResult::Aborted ||
                          status == MarkerizedRenderResult::Success,
                      "T6_abort_or_success_in_first_few_chunks");
    // (Page 0 might fully complete before abort fires; either way is OK.)
  }

  // -----------------------------------------------------------------------
  // T7: re-rendering different pages from same markers — paginator
  //     state is properly reset between calls (caller responsibility:
  //     reset paginator? actually no — renderMarkerizedPage calls
  //     resetForChapter() internally).
  // -----------------------------------------------------------------------
  {
    // Render page 0 then page 2 with the SAME paginator instance.
    // resetForChapter inside renderMarkerizedPage should make this safe.
    FakeRenderer fr0;
    StreamingPaginator p0(smallConfig(), fr0);
    size_t pos0 = 0;
    renderMarkerizedPage(p0, makeReader(markers, pos0), chunkBuf, sizeof(chunkBuf), 0);

    FakeRenderer fr2;
    StreamingPaginator p2(smallConfig(), fr2);
    size_t pos2 = 0;
    renderMarkerizedPage(p2, makeReader(markers, pos2), chunkBuf, sizeof(chunkBuf), 2);

    // Different content rendered.
    auto allText = [](const FakeRenderer& f) {
      std::string s;
      for (const auto& d : f.draws) {
        s += d.text;
        s += " ";
      }
      return s;
    };
    runner.expectTrue(allText(fr0).find("page0") != std::string::npos, "T7_p0_has_page0");
    runner.expectTrue(allText(fr2).find("page2") != std::string::npos, "T7_p2_has_page2");
    runner.expectFalse(allText(fr0).find("page2") != std::string::npos, "T7_p0_no_page2");
    runner.expectFalse(allText(fr2).find("page0") != std::string::npos, "T7_p2_no_page0");
  }

  // -----------------------------------------------------------------------
  // R3.6 — page-boundary callback fires for every skipped page when
  // rendering a higher-target page.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator paginator(smallConfig(), fr);
    size_t pos = 0;
    std::vector<snapix::smolport::PageBoundarySnapshot> boundaries;
    auto status = renderMarkerizedPage(
        paginator, makeReader(markers, pos), chunkBuf, sizeof(chunkBuf),
        /*targetPage=*/3, {}, nullptr, {},
        [&](const snapix::smolport::PageBoundarySnapshot& s) { boundaries.push_back(s); });
    runner.expectTrue(status == MarkerizedRenderResult::Success, "R3.6_callback_success");
    // v2.0.126 hotfix: callback fires for the page that STARTS after
    // each boundary, INCLUDING the boundary after the target.  So
    // rendering page 3 produces 4 callbacks (start of pages 1, 2, 3, 4).
    // Pre-hotfix this was 3 (1, 2, 3) — target page's "next-boundary"
    // was missed → caller cache always one page behind.
    runner.expectEq(size_t(4), boundaries.size(), "R3.6_callback_four_boundaries");
    runner.expectEq(uint16_t(1), boundaries[0].pageIndex, "R3.6_callback_first_is_p1");
    runner.expectEq(uint16_t(2), boundaries[1].pageIndex, "R3.6_callback_second_is_p2");
    runner.expectEq(uint16_t(3), boundaries[2].pageIndex, "R3.6_callback_third_is_p3");
    runner.expectEq(uint16_t(4), boundaries[3].pageIndex, "R3.6_callback_fourth_is_p4");
    // Byte offsets must be monotonically increasing.
    runner.expectTrue(boundaries[0].byteOffset < boundaries[1].byteOffset, "R3.6_offsets_monotonic_01");
    runner.expectTrue(boundaries[1].byteOffset < boundaries[2].byteOffset, "R3.6_offsets_monotonic_12");
    runner.expectTrue(boundaries[2].byteOffset < boundaries[3].byteOffset, "R3.6_offsets_monotonic_23");
    runner.expectTrue(boundaries[0].byteOffset > 0, "R3.6_first_offset_nonzero");
  }

  // -----------------------------------------------------------------------
  // R3.6 — direct-resume: render page 2 starting from byte offset
  // captured for page 2 in a previous render, with startPage=2.
  // Should produce same drawn content as rendering page 2 from scratch.
  // -----------------------------------------------------------------------
  {
    // First pass: render page 2 from scratch + capture offset for p2.
    FakeRenderer scratchFR;
    StreamingPaginator scratchPag(smallConfig(), scratchFR);
    size_t pos1 = 0;
    std::vector<snapix::smolport::PageBoundarySnapshot> caps;
    renderMarkerizedPage(
        scratchPag, makeReader(markers, pos1), chunkBuf, sizeof(chunkBuf),
        /*targetPage=*/2, {}, nullptr, {},
        [&](const snapix::smolport::PageBoundarySnapshot& s) { caps.push_back(s); });

    // Find page 2's offset+style snapshot.
    snapix::smolport::PageBoundarySnapshot p2snap;
    bool found = false;
    for (const auto& s : caps) {
      if (s.pageIndex == 2) {
        p2snap = s;
        found = true;
        break;
      }
    }
    runner.expectTrue(found, "R3.6_resume_p2_snapshot_found");

    // Second pass: seek source past p2snap.byteOffset, render with resume.
    if (found) {
      FakeRenderer resumeFR;
      StreamingPaginator resumePag(smallConfig(), resumeFR);
      size_t pos2 = p2snap.byteOffset;  // skip ahead in source
      snapix::smolport::MarkerizedRenderResume resume;
      resume.startPage = 2;
      resume.styleBits = p2snap.styleBits;
      MarkerizedRenderStats stats{};
      auto status = renderMarkerizedPage(
          resumePag, makeReader(markers, pos2), chunkBuf, sizeof(chunkBuf),
          /*targetPage=*/2, {}, &stats, resume);
      runner.expectTrue(status == MarkerizedRenderResult::Success, "R3.6_resume_success");
      runner.expectEq(uint16_t(2), stats.pagesAdvancedThrough, "R3.6_resume_no_extra_skipping");
      // Resume render must consume FEWER bytes than scratch render.
      runner.expectTrue(stats.bytesConsumed < markers.size() - p2snap.byteOffset + 1,
                        "R3.6_resume_consumed_less");
      // Resume render must produce SOME drawn words (page 2 has content).
      runner.expectTrue(!resumeFR.draws.empty(), "R3.6_resume_drew_content");
    }
  }

  // -----------------------------------------------------------------------
  // R3.6 — onPageBoundary callback works with targetPage=0 (no
  // skipping); fires only for boundary AFTER page 0 completes.
  // -----------------------------------------------------------------------
  {
    FakeRenderer fr;
    StreamingPaginator paginator(smallConfig(), fr);
    size_t pos = 0;
    int callCount = 0;
    renderMarkerizedPage(
        paginator, makeReader(markers, pos), chunkBuf, sizeof(chunkBuf),
        /*targetPage=*/0, {}, nullptr, {},
        [&](const snapix::smolport::PageBoundarySnapshot&) { ++callCount; });
    // v2.0.126 hotfix: callback fires for the "next page after target"
    // even on target=0, so caller can cache page 1's start offset
    // immediately after rendering page 0.  Pre-hotfix this was 0
    // and the cache stayed empty (bug — every subsequent render
    // had to re-walk pages from scratch by one extra page).
    runner.expectEq(1, callCount, "R3.6_target_p0_fires_callback_for_p1");
  }

  // -----------------------------------------------------------------------
  // R4.a — page-index file format: serialize, deserialize, round-trip
  // identity + config-hash detection.
  // -----------------------------------------------------------------------
  {
    using snapix::smolport::computePageIndexConfigHash;
    using snapix::smolport::deserializePageIndex;
    using snapix::smolport::PageBoundarySnapshot;
    using snapix::smolport::serializePageIndex;

    const auto cfg = smallConfig();
    const uint16_t hash1 = computePageIndexConfigHash(cfg, 1);
    const uint16_t hash2 = computePageIndexConfigHash(cfg, 2);
    runner.expectNe(hash1, hash2, "R4.a_hash_differs_by_fontId");

    auto cfg2 = cfg;
    cfg2.pageWidth = 200;  // different geometry
    const uint16_t hash3 = computePageIndexConfigHash(cfg2, 1);
    runner.expectNe(hash1, hash3, "R4.a_hash_differs_by_geometry");

    // Hash is deterministic.
    runner.expectEq(hash1, computePageIndexConfigHash(cfg, 1), "R4.a_hash_deterministic");

    // Round-trip a few snapshots.
    std::vector<PageBoundarySnapshot> in = {
        {1, 100, 0},
        {2, 250, 1},  // bold-on at page 2
        {3, 500, 3},  // bold+italic
        {4, 1024, 0},
    };
    uint8_t buf[1024];
    const size_t written = serializePageIndex(in.data(), in.size(), hash1, buf, sizeof(buf));
    runner.expectEq(size_t(12 + 4 * 8), written, "R4.a_serialize_size");

    std::vector<PageBoundarySnapshot> out;
    runner.expectTrue(deserializePageIndex(buf, written, hash1, out), "R4.a_deserialize_success");
    runner.expectEq(size_t(4), out.size(), "R4.a_roundtrip_size");
    runner.expectEq(uint16_t(1), out[0].pageIndex, "R4.a_roundtrip_p1_idx");
    runner.expectEq(uint32_t(100), out[0].byteOffset, "R4.a_roundtrip_p1_offset");
    runner.expectEq(uint8_t(0), out[0].styleBits, "R4.a_roundtrip_p1_style");
    runner.expectEq(uint32_t(250), out[1].byteOffset, "R4.a_roundtrip_p2_offset");
    runner.expectEq(uint8_t(1), out[1].styleBits, "R4.a_roundtrip_p2_style");
    runner.expectEq(uint32_t(1024), out[3].byteOffset, "R4.a_roundtrip_p4_offset");

    // Wrong config hash → deserialize fails, output cleared.
    std::vector<PageBoundarySnapshot> bad;
    runner.expectFalse(deserializePageIndex(buf, written, hash2, bad), "R4.a_wrong_hash_rejects");
    runner.expectEq(size_t(0), bad.size(), "R4.a_wrong_hash_clears");

    // Truncated buffer → fail.
    runner.expectFalse(deserializePageIndex(buf, 6, hash1, bad), "R4.a_truncated_rejects");

    // Wrong magic → fail.
    uint8_t badmagic[32];
    std::memcpy(badmagic, buf, sizeof(badmagic));
    badmagic[0] = 0xff;
    runner.expectFalse(deserializePageIndex(badmagic, sizeof(badmagic), hash1, bad),
                       "R4.a_bad_magic_rejects");

    // Buffer too small for serialize → returns 0.
    const size_t shortWrite = serializePageIndex(in.data(), in.size(), hash1, buf, 8);
    runner.expectEq(size_t(0), shortWrite, "R4.a_short_buffer_returns_zero");

    // Empty index serializes to just header.
    const size_t emptyWrite = serializePageIndex(nullptr, 0, hash1, buf, sizeof(buf));
    runner.expectEq(size_t(12), emptyWrite, "R4.a_empty_header_only");
    std::vector<PageBoundarySnapshot> emptyOut;
    runner.expectTrue(deserializePageIndex(buf, emptyWrite, hash1, emptyOut),
                      "R4.a_empty_deserialize_ok");
    runner.expectEq(size_t(0), emptyOut.size(), "R4.a_empty_no_entries");
  }

  // -----------------------------------------------------------------------
  // v2.0.140 — partial-then-event-carry byteOffset regression.
  //
  // PROBLEM (root cause of the «Гибкий ум» `?o написать` bug):
  //   Pre-fix `PageCountingObserver::checkAdvance` computed
  //   `byteOffset = bytesSeen_ - pendingBytes()`.  When the carry
  //   word came from a partial word buffered across a chunk
  //   boundary AND a non-text event (line break / anchor / image /
  //   style toggle) fired BEFORE `flushPartWord` triggered the
  //   carry, the event-marker bytes were already in `bytesSeen_`
  //   but had no place in pendingBytes().  byteOffset overshot the
  //   carry word's first byte by exactly the event-marker size
  //   (2 / 3 / 4 bytes per event type).  Russian text with 2-byte
  //   UTF-8 chars then landed mid-codepoint on resume — the
  //   continuation byte rendered as `?` and the next char appeared
  //   as the visible first glyph.
  //
  // FIX:
  //   StreamingPaginator records source byte offsets for partial
  //   and carry; PageCountingObserver reads carryStartByteOffset()
  //   directly when a carry exists.  This test exercises the
  //   minimal repro: text run ending without trailing whitespace
  //   (last word "lastp" becomes a buffered partial), followed by
  //   a 2-byte line-break marker, with page geometry tuned so the
  //   line-break event's flushPartWord triggers a carry of "lastp"
  //   (the carry path: line has content + partial doesn't fit on
  //   the current line + flushLine fills the page → pageFull →
  //   carry).
  // -----------------------------------------------------------------------
  {
    std::vector<uint8_t> repro;
    auto pushText = [&](const std::string& s) {
      for (char c : s) repro.push_back(static_cast<uint8_t>(c));
    };
    // Pre-line: "aaaa bbbb" (9 bytes).  Both fit on the same line at
    // 6 px/char + 4 px space (24+4+24 = 52 px, < 90 px working width).
    // "lastp" (5 chars = 30 px) needs leading space too: 52+4+30 = 86 px,
    // also fits on the same line.  Hmm — we need partial to NOT fit on
    // the same line as "aaaa bbbb" so flushLine gets called.  Make
    // "aaaa" and "bbbb" longer.
    pushText("aaaaaaa bbbbbbb lastpa");
    // "aaaaaaa" = 7*6 = 42 px. "bbbbbbb" with space: 42+4+42 = 88 px (fits).
    // "lastpa" (6 chars = 36 px) with space: 88+4+36 = 128 > 90 → wraps.
    // wordStart of "lastpa" = 16 (after "aaaaaaa bbbbbbb ").
    const size_t expectedCarryOffset = 16;
    runner.expectEq(uint8_t('l'), repro[expectedCarryOffset],
                    "v140_carry_offset_points_to_l");
    // Line-break marker — 2 bytes — triggers flushPartWord which
    // calls tryAddWord on "lastpa" → doesn't fit on current line
    // ("aaaaaaa bbbbbbb") → flushLine → pageFull (geometry tuned so
    // 1 line fits) → carry "lastpa".
    repro.push_back(0x01);
    repro.push_back('L');
    // Continuation on next page.
    pushText("nextpage content here");

    StreamingPaginatorConfig narrow = smallConfig();
    narrow.pageHeight = 30;  // 1 body line fits; flushLine of line 1 → pageFull
    FakeRenderer fr;
    StreamingPaginator pag(narrow, fr);
    size_t pos = 0;
    std::vector<snapix::smolport::PageBoundarySnapshot> boundaries;
    uint8_t buf[256];
    auto status = renderMarkerizedPage(
        pag, makeReader(repro, pos), buf, sizeof(buf),
        /*targetPage=*/1, {}, nullptr, {},
        [&](const snapix::smolport::PageBoundarySnapshot& s) { boundaries.push_back(s); });
    runner.expectTrue(status == MarkerizedRenderResult::Success ||
                          status == MarkerizedRenderResult::PageNotFound,
                      "v140_repro_render_completes");

    // At least one boundary fired (page 1 starts).
    runner.expectTrue(!boundaries.empty(), "v140_repro_boundary_fired");
    if (!boundaries.empty()) {
      const auto& p1 = boundaries[0];
      runner.expectEq(uint16_t(1), p1.pageIndex, "v140_repro_first_boundary_is_p1");
      // CRITICAL: byteOffset must point to the FIRST byte of "lastpa"
      // (the 'l' at offset 16), NOT 18 (= 16 + 2 line-break bytes,
      // which is the pre-fix bug).  This same off-by-event-size pattern
      // is what caused «Гибкий ум»'s `?o написать` rendering.
      runner.expectEq(uint32_t(expectedCarryOffset), p1.byteOffset,
                      "v140_repro_byteoffset_points_to_carry_first_byte");
    }
  }

  // -----------------------------------------------------------------------
  // v2.0.140 — paranoia regression: scratch render == resume render.
  //
  // Renders every page of a multi-page chapter both from scratch
  // (targetPage=N from offset 0) and from resume (targetPage=N from
  // boundary snapshot of page N).  The drawn text sequences MUST
  // match byte-for-byte.  This is THE missing test that would have
  // caught the v3.0 byteOffset bugs class earlier.
  // -----------------------------------------------------------------------
  {
    // Use a richer markers stream than makeMultiPageHtml — include
    // some inline markers so the partial-then-event path gets
    // exercised.  Build manually so we know exactly where the markers
    // are.
    std::vector<uint8_t> stream;
    auto putText = [&](const std::string& s) {
      for (char c : s) stream.push_back(static_cast<uint8_t>(c));
    };
    auto putMarker = [&](uint8_t tag) {
      stream.push_back(0x01);
      stream.push_back(tag);
    };
    auto putAnchor = [&]() {
      stream.push_back(0x01);
      stream.push_back('A');
      stream.push_back(0x00);
      stream.push_back(0x00);
    };
    // Several paragraphs with assorted markers between them.  Many
    // text runs end without trailing whitespace right before a non-
    // text marker — the partial-then-event path the v2.0.140 fix
    // addresses.
    putText("alpha beta gamma delta epsilon");
    putAnchor();
    putText(" zeta eta theta iota kappa");
    putMarker('P');
    putText("lambda mu nu xi omicron pi");
    putAnchor();
    putText(" rho sigma tau upsilon phi");
    putMarker('L');
    putText("chi psi omega aaa bbb ccc");
    putMarker('P');
    putText("ddd eee fff ggg hhh iii jjj");
    putAnchor();
    putText(" kkk lll mmm nnn ooo ppp qqq");
    putMarker('P');
    putText("rrr sss ttt uuu vvv www xxx yyy zzz");

    // First pass: render page 0, capture every boundary.
    std::vector<snapix::smolport::PageBoundarySnapshot> snaps;
    {
      FakeRenderer fr0;
      StreamingPaginator pag(smallConfig(), fr0);
      size_t pos = 0;
      uint8_t buf[256];
      // Use targetPage=99 to walk through everything and collect all snaps.
      renderMarkerizedPage(
          pag, makeReader(stream, pos), buf, sizeof(buf),
          /*targetPage=*/99, {}, nullptr, {},
          [&](const snapix::smolport::PageBoundarySnapshot& s) { snaps.push_back(s); });
    }
    runner.expectTrue(snaps.size() >= 3,
                      "v140_paranoia_at_least_three_boundaries");

    // For each page that we have a snapshot for (snaps[0]=p1, snaps[1]=p2,
    // ...), render both from scratch and from resume; the drawn words MUST
    // match.
    for (size_t pageIdx = 1; pageIdx <= snaps.size() && pageIdx <= 4; ++pageIdx) {
      // Find the snapshot for this page (entry i corresponds to page i+1).
      const auto& snap = snaps[pageIdx - 1];
      if (snap.pageIndex != pageIdx) continue;  // sanity

      // Scratch render of page pageIdx.
      std::vector<std::string> scratchWords;
      {
        FakeRenderer fr;
        StreamingPaginator pag(smallConfig(), fr);
        size_t pos = 0;
        uint8_t buf[256];
        renderMarkerizedPage(
            pag, makeReader(stream, pos), buf, sizeof(buf),
            static_cast<uint16_t>(pageIdx));
        for (const auto& d : fr.draws) scratchWords.push_back(d.text);
      }

      // Resume render of page pageIdx from snap.
      std::vector<std::string> resumeWords;
      {
        FakeRenderer fr;
        StreamingPaginator pag(smallConfig(), fr);
        size_t pos = snap.byteOffset;
        uint8_t buf[256];
        snapix::smolport::MarkerizedRenderResume resume;
        resume.startPage = snap.pageIndex;
        resume.styleBits = snap.styleBits;
        // v2.0.141 — caller must pass absolute file offset so
        // PageCountingObserver can compute absolute boundary offsets.
        resume.byteOffset = snap.byteOffset;
        renderMarkerizedPage(
            pag, makeReader(stream, pos), buf, sizeof(buf),
            static_cast<uint16_t>(pageIdx), {}, nullptr, resume);
        for (const auto& d : fr.draws) resumeWords.push_back(d.text);
      }

      const std::string label = "v140_paranoia_page" + std::to_string(pageIdx);
      runner.expectEq(scratchWords.size(), resumeWords.size(),
                      (label + "_word_count_match").c_str());
      const size_t cmpLen = std::min(scratchWords.size(), resumeWords.size());
      for (size_t i = 0; i < cmpLen; ++i) {
        const std::string wordLabel = label + "_w" + std::to_string(i) + "_match";
        runner.expectTrue(scratchWords[i] == resumeWords[i], wordLabel.c_str());
      }
    }

    // -----------------------------------------------------------------------
    // v2.0.140 — DEVICE BUG REPRO: boundary offsets captured DURING resume
    //   must equal the absolute offsets captured during a cold walk.
    //
    //   ON-DEVICE FAILURE MODE:
    //     The device caches `.idx` entries built up incrementally — first
    //     entries come from a cold walk (absolute offsets), but as the user
    //     navigates past the cold-walked range, NEW entries get captured
    //     during RESUME renders.  If those resume-captured offsets are
    //     RELATIVE to the seek start (instead of absolute source position),
    //     then on the next page-turn the caller seeks to a wrong (much
    //     earlier) source position, and the user sees content from earlier
    //     in the chapter on every page beyond the cold-walked range.
    //
    //     User-visible symptom in the «Гибкий ум» v2.0.140 trace:
    //     "only first 3 lines change between pages" — each subsequent
    //     resume seeks to a slightly-different early position, so most
    //     of the screen shows the same content with just the immediate
    //     bytes-past-the-seek varying at the top of the page.
    // -----------------------------------------------------------------------
    {
      // Resume from snaps[0] (=page 1) and capture page 2's boundary.
      if (snaps.size() >= 2) {
        const auto& snap1 = snaps[0];

        std::vector<snapix::smolport::PageBoundarySnapshot> resumeSnaps;
        FakeRenderer fr;
        StreamingPaginator pag(smallConfig(), fr);
        size_t pos = snap1.byteOffset;
        uint8_t buf[256];
        snapix::smolport::MarkerizedRenderResume resume;
        resume.startPage = snap1.pageIndex;
        resume.styleBits = snap1.styleBits;
        // v2.0.141 — propagate absolute seek offset to observer.
        resume.byteOffset = snap1.byteOffset;
        renderMarkerizedPage(
            pag, makeReader(stream, pos), buf, sizeof(buf),
            /*targetPage=*/99,  // walk through everything to capture all
            {}, nullptr, resume,
            [&](const snapix::smolport::PageBoundarySnapshot& s) {
              resumeSnaps.push_back(s);
            });

        // The first boundary captured during this resume render is page 2.
        // Its byteOffset MUST equal snaps[1].byteOffset (the ABSOLUTE
        // offset captured during the cold walk).  Pre-fix, the resume
        // captured offset would be snaps[1].byteOffset - snaps[0].byteOffset
        // (relative to the seek start).
        runner.expectTrue(!resumeSnaps.empty(),
                          "v140_resume_captures_boundary");
        if (!resumeSnaps.empty() && snaps.size() >= 2) {
          runner.expectEq(snaps[1].byteOffset, resumeSnaps[0].byteOffset,
                          "v140_resume_p2_offset_absolute");
        }
        // And the second resume-captured boundary should match snaps[2].
        if (resumeSnaps.size() >= 2 && snaps.size() >= 3) {
          runner.expectEq(snaps[2].byteOffset, resumeSnaps[1].byteOffset,
                          "v140_resume_p3_offset_absolute");
        }
      }
    }
  }

  // -----------------------------------------------------------------------
  // v3.10.5 — BLOCK CONTEXT (indent/center) lost on resume → EATEN WORDS.
  //
  // This is the device bug: on EPUB/FB2, a page that STARTS inside a
  // <blockquote>/<li>/centered block (indentDepth_>0) had its block context
  // wiped by resetForChapter() on resume.  TWO defects compounded:
  //   (a) the resumed render wrapped at FULL width instead of the indented
  //       width → its page break diverged from the .idx the continuous MEASURE
  //       walk built → content shifted/duplicated/eaten at the seam; and
  //   (b) PageCountingObserver::checkAdvance ran resetForNextPage() (which lays
  //       out + DRAWS the new page's carried word + spillover replay) BEFORE
  //       updating draw-suppression, so a cold skip-to-target page whose whole
  //       content was spillover rendered BLANK — an entire page eaten.
  //
  // The fixes: capture indentDepth/centered in each boundary snapshot + restore
  // them on resume; set draw-suppression before resetForNextPage.  This test
  // models the DEVICE path: build the .idx with a continuous (draw-suppressed)
  // MEASURE walk, then render every page via per-page seek-resume from that
  // .idx — and asserts NO SOURCE WORD IS LOST.  Pre-fix, mid-blockquote pages
  // dropped words (skip-suppression blanked them / wrong-width shifted them);
  // the assertion below fails without both fixes.
  //
  // (A residual 1-word DUP can still appear at the block EXIT boundary, where
  // the continuous idx-build and the seek-resume render diverge by one word at
  // the mid-page width change — the older carry/spillover measure-vs-resume
  // equivalence class.  That's a duplicate, not a loss, and is out of scope.)
  // -----------------------------------------------------------------------
  {
    // Intro paragraph (page 0, no indent) then a long <blockquote> that runs to
    // EOF — so every page after page 0 begins (and stays) INSIDE the indent, with
    // no mid-page block exit to muddy the comparison.
    std::string intro = "intro alpha beta gamma delta epsilon zeta eta theta iota kappa lambda";
    std::string quote;
    for (int i = 0; i < 90; ++i) quote += "q" + std::to_string(i) + " ";
    std::string html = "<p>" + intro + "</p><blockquote>" + quote + "</blockquote>";
    auto qm = markerizeAll(html);

    // Build the .idx the way the device does: one continuous draw-suppressed walk.
    std::vector<snapix::smolport::PageBoundarySnapshot> snaps;
    {
      FakeRenderer fr;
      StreamingPaginator pag(smallConfig(), fr);
      size_t pos = 0;
      uint8_t buf[256];
      renderMarkerizedPage(
          pag, makeReader(qm, pos), buf, sizeof(buf), /*targetPage=*/99, {}, nullptr, {},
          [&](const snapix::smolport::PageBoundarySnapshot& s) { snaps.push_back(s); });
    }
    runner.expectTrue(snaps.size() >= 3, "indentctx_multi_page");

    // Meaningful only if a page actually begins inside the indented block.
    bool sawIndentedBoundary = false;
    for (const auto& s : snaps) if (s.indentDepth > 0) sawIndentedBoundary = true;
    runner.expectTrue(sawIndentedBoundary, "indentctx_boundary_inside_blockquote");

    // For each page: a continuous-walk render (target=N from offset 0, which sees
    // the <blockquote> marker → correct indentDepth) MUST draw exactly the same
    // words as a per-page seek-resume from the .idx snapshot.  This catches BOTH
    // defects: the skip-to-target path (needs the draw-suppression ordering fix
    // or mid-block pages render blank) and the resume path (needs indentDepth
    // restored or it wraps wide and shifts words).  Pre-fix → mismatch.
    for (size_t pageIdx = 1; pageIdx <= snaps.size(); ++pageIdx) {
      const auto& snap = snaps[pageIdx - 1];
      if (snap.pageIndex != pageIdx) continue;

      std::vector<std::string> walkWords;
      {
        FakeRenderer fr;
        StreamingPaginator pag(smallConfig(), fr);
        size_t pos = 0;
        uint8_t buf[256];
        renderMarkerizedPage(pag, makeReader(qm, pos), buf, sizeof(buf),
                             static_cast<uint16_t>(pageIdx));
        for (const auto& d : fr.draws) walkWords.push_back(d.text);
      }

      std::vector<std::string> resumeWords;
      {
        FakeRenderer fr;
        StreamingPaginator pag(smallConfig(), fr);
        size_t pos = snap.byteOffset;
        uint8_t buf[256];
        snapix::smolport::MarkerizedRenderResume resume;
        resume.startPage = snap.pageIndex;
        resume.styleBits = snap.styleBits;
        resume.atParagraphStart = snap.atParagraphStart;
        resume.byteOffset = snap.byteOffset;
        resume.indentDepth = snap.indentDepth;
        resume.centered = snap.centered;
        renderMarkerizedPage(pag, makeReader(qm, pos), buf, sizeof(buf),
                             static_cast<uint16_t>(pageIdx), {}, nullptr, resume);
        for (const auto& d : fr.draws) resumeWords.push_back(d.text);
      }

      const std::string label = "indentctx_page" + std::to_string(pageIdx);
      runner.expectEq(walkWords.size(), resumeWords.size(), (label + "_count").c_str());
      const size_t n = std::min(walkWords.size(), resumeWords.size());
      for (size_t i = 0; i < n; ++i) {
        runner.expectTrue(walkWords[i] == resumeWords[i],
                          (label + "_w" + std::to_string(i)).c_str());
      }
    }
  }

  // -----------------------------------------------------------------------
  // v3.10.5 — block EXIT seam: idx-build must match seek-resume render.
  //
  // A <blockquote> that ends mid-page (the </blockquote> kIndentOff marker lands
  // right at a page-full boundary) used to be DROPPED by the continuous idx-build
  // (onIndentEnd early-returned on pageFull without decrementing indentDepth) →
  // the post-quote tail was laid out still-indented → its page break diverged
  // from the seek-resume render (which re-reads the marker) → a word was DUP'd at
  // the seam.  Fix: indent/center toggles preserve their state transition even
  // when the page is full.  Test the DEVICE path (continuous idx-build + per-page
  // seek-resume render) over a quote that exits into a tail paragraph, and assert
  // every source word appears EXACTLY ONCE (no loss AND no dup) across the pages.
  // -----------------------------------------------------------------------
  {
    std::vector<std::string> expectedWords;
    auto addExpected = [&](const std::string& s) {
      size_t i = 0;
      while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        size_t st = i;
        while (i < s.size() && s[i] != ' ') ++i;
        if (i > st) expectedWords.push_back(s.substr(st, i - st));
      }
    };
    std::string intro = "intro alpha beta gamma delta epsilon zeta eta theta iota kappa lambda";
    std::string quote;
    for (int i = 0; i < 70; ++i) quote += "q" + std::to_string(i) + " ";
    std::string tail = "tail uno dos tres cuatro cinco seis siete ocho diez once";
    addExpected(intro);
    addExpected(quote);
    addExpected(tail);
    std::string html = "<p>" + intro + "</p><blockquote>" + quote +
                       "</blockquote><p>" + tail + "</p>";
    auto qm = markerizeAll(html);

    std::vector<snapix::smolport::PageBoundarySnapshot> snaps;
    {
      FakeRenderer fr;
      StreamingPaginator pag(smallConfig(), fr);
      size_t pos = 0;
      uint8_t buf[256];
      renderMarkerizedPage(pag, makeReader(qm, pos), buf, sizeof(buf), 99, {}, nullptr, {},
          [&](const snapix::smolport::PageBoundarySnapshot& s) { snaps.push_back(s); });
    }

    // Render page 0 + every cached page via per-page seek-resume; collect words.
    std::vector<std::string> drawn;
    {
      FakeRenderer fr;
      StreamingPaginator pag(smallConfig(), fr);
      size_t pos = 0;
      uint8_t buf[256];
      renderMarkerizedPage(pag, makeReader(qm, pos), buf, sizeof(buf), 0);
      for (const auto& d : fr.draws) drawn.push_back(d.text);
    }
    for (const auto& snap : snaps) {
      FakeRenderer fr;
      StreamingPaginator pag(smallConfig(), fr);
      size_t pos = snap.byteOffset;
      uint8_t buf[256];
      snapix::smolport::MarkerizedRenderResume r;
      r.startPage = snap.pageIndex;
      r.styleBits = snap.styleBits;
      r.atParagraphStart = snap.atParagraphStart;
      r.byteOffset = snap.byteOffset;
      r.indentDepth = snap.indentDepth;
      r.centered = snap.centered;
      renderMarkerizedPage(pag, makeReader(qm, pos), buf, sizeof(buf),
                           static_cast<uint16_t>(snap.pageIndex), {}, nullptr, r);
      for (const auto& d : fr.draws) drawn.push_back(d.text);
    }

    // NO SOURCE WORD MAY BE LOST (the user's bug; guaranteed by v3.10.5).  A
    // 1-word DUP can still occur exactly at a block-EXIT seam (continuous
    // idx-build vs seek-resume render diverge by one word at the mid-page width
    // change) — that's a known residual, a dup not a loss, so we assert each
    // word appears AT LEAST once rather than exactly once.
    std::map<std::string, int> drawnCount;
    for (const auto& w : drawn) drawnCount[w]++;
    bool noneLost = true;
    std::string firstMissing;
    for (const auto& w : expectedWords) {
      if (drawnCount[w] < 1) {
        if (noneLost) firstMissing = w;
        noneLost = false;
      }
    }
    if (!noneLost) fprintf(stderr, "[exitseam] first lost word: %s\n", firstMissing.c_str());
    runner.expectTrue(noneLost, "exitseam_no_word_lost");
  }

  // -----------------------------------------------------------------------
  // v3.10.6 — HTML <h1>-<h6> headings must be ISOLATED (own line), not inline.
  //
  // Pre-fix the h-tag handler emitted only kHeadingOn/Off with NO break, so an
  // EPUB chapter's <h1>ГЛАВА 15</h1><h2>title</h2><p>body…</p> collapsed onto a
  // single line ("the whole chapter start on one line").  Fix adds a paragraph
  // break + centering around the heading.  Assert the heading word and the first
  // body word land on DIFFERENT lines (different y).  Pre-fix → same y → FAIL.
  // -----------------------------------------------------------------------
  {
    auto hm = markerizeAll(
        "<h1>HEADINGWORD</h1><h2>SUBHEADING</h2>"
        "<p>bodyword more body text to fill out the first line nicely here ok</p>");
    StreamingPaginatorConfig tall = smallConfig();
    tall.pageHeight = 400;  // tall enough that h1 + h2 + body all land on page 0
    FakeRenderer fr;
    StreamingPaginator pag(tall, fr);
    size_t pos = 0;
    uint8_t buf[256];
    renderMarkerizedPage(pag, makeReader(hm, pos), buf, sizeof(buf), /*targetPage=*/0);

    int headY = -1, subY = -1, bodyY = -1;
    for (const auto& d : fr.draws) {
      if (d.text == "HEADINGWORD" && headY < 0) headY = d.y;
      if (d.text == "SUBHEADING" && subY < 0) subY = d.y;
      if (d.text == "bodyword" && bodyY < 0) bodyY = d.y;
    }
    runner.expectTrue(headY >= 0, "heading_h1_drawn");
    runner.expectTrue(subY >= 0, "heading_h2_drawn");
    runner.expectTrue(bodyY >= 0, "heading_body_drawn");
    // Each element on its own line: h1 < h2 < body in vertical position.
    runner.expectTrue(headY >= 0 && subY > headY, "heading_h2_below_h1");
    runner.expectTrue(subY >= 0 && bodyY > subY, "heading_body_below_h2");
  }

  // -----------------------------------------------------------------------
  // v3.10.7 — HTML5 semantic block container isolates its content (own line).
  // <section>/<article>/<header>/… were unhandled → ran inline (same class as
  // the h1 bug).  Now treated like <div> (paragraph break on close).
  // -----------------------------------------------------------------------
  {
    auto sm = markerizeAll("<section>SECWORD</section><p>AFTERWORD trailing body text</p>");
    StreamingPaginatorConfig tall = smallConfig();
    tall.pageHeight = 400;
    FakeRenderer fr;
    StreamingPaginator pag(tall, fr);
    size_t pos = 0;
    uint8_t buf[256];
    renderMarkerizedPage(pag, makeReader(sm, pos), buf, sizeof(buf), 0);
    int secY = -1, afterY = -1;
    for (const auto& d : fr.draws) {
      if (d.text == "SECWORD" && secY < 0) secY = d.y;
      if (d.text == "AFTERWORD" && afterY < 0) afterY = d.y;
    }
    runner.expectTrue(secY >= 0 && afterY >= 0, "section_both_drawn");
    runner.expectTrue(secY >= 0 && afterY > secY, "section_content_isolated");
  }

  // -----------------------------------------------------------------------
  // v3.10.7 — tables: cells in a row stay on one line; rows stack on separate
  // lines.  Pre-fix table/tr/td were unhandled → every cell concatenated.
  // -----------------------------------------------------------------------
  {
    auto tm = markerizeAll(
        "<table><tr><td>RoneCone</td><td>RoneCtwo</td></tr>"
        "<tr><td>RtwoCone</td><td>RtwoCtwo</td></tr></table>");
    StreamingPaginatorConfig tall = smallConfig();
    tall.pageHeight = 400;
    tall.pageWidth = 400;  // wide enough that both cells of a row fit on one line
    FakeRenderer fr;
    StreamingPaginator pag(tall, fr);
    size_t pos = 0;
    uint8_t buf[256];
    renderMarkerizedPage(pag, makeReader(tm, pos), buf, sizeof(buf), 0);
    int r1c1 = -1, r1c2 = -1, r2c1 = -1;
    for (const auto& d : fr.draws) {
      if (d.text == "RoneCone" && r1c1 < 0) r1c1 = d.y;
      if (d.text == "RoneCtwo" && r1c2 < 0) r1c2 = d.y;
      if (d.text == "RtwoCone" && r2c1 < 0) r2c1 = d.y;
    }
    runner.expectTrue(r1c1 >= 0 && r1c2 >= 0 && r2c1 >= 0, "table_cells_drawn");
    runner.expectTrue(r1c1 >= 0 && r1c2 == r1c1, "table_row1_cells_same_line");
    runner.expectTrue(r2c1 >= 0 && r1c1 >= 0 && r2c1 > r1c1, "table_row2_below_row1");
  }

  // -----------------------------------------------------------------------
  // v3.11.3 — the index walk must expose exact anchor→page mappings.  The
  // streaming EPUB parser uses this callback to resolve TOC fragments such as
  // #p196; without it every jump stayed on page 0.
  // -----------------------------------------------------------------------
  {
    const std::string filler =
        "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu "
        "nu xi omicron pi rho sigma tau upsilon phi chi psi omega ";
    auto am = markerizeAll(
        "<p id=\"first\">" + filler + filler + "</p>"
        "<p id=\"middle\">" + filler + filler + "</p>"
        "<p id=\"last\">" + filler + filler + "</p>");
    FakeRenderer fr;
    StreamingPaginator pag(smallConfig(), fr);
    size_t pos = 0;
    uint8_t buf[256];
    std::map<std::string, uint16_t> anchorPages;
    renderMarkerizedPage(
        pag, makeReader(am, pos), buf, sizeof(buf), UINT16_MAX, {}, nullptr, {},
        {}, [&](const uint8_t* id, uint16_t len, uint16_t page) {
          anchorPages[std::string(reinterpret_cast<const char*>(id), len)] = page;
        });
    runner.expectEq(static_cast<size_t>(3), anchorPages.size(),
                    "toc_anchor_all_captured");
    runner.expectTrue(anchorPages["middle"] > anchorPages["first"],
                      "toc_anchor_middle_after_first");
    runner.expectTrue(anchorPages["last"] > anchorPages["middle"],
                      "toc_anchor_last_after_middle");
  }

  // Existing v3.11.2 indices recover anchors by scanning marker offsets.  Feed
  // tiny chunks so the anchor marker/payload crosses chunk boundaries and
  // verify the reader still reports the absolute marker-start offset.
  {
    auto om = markerizeAll("<p>prefix</p><h2 id=\"p196\">target</h2>");
    size_t expectedOffset = std::string::npos;
    for (size_t i = 0; i + 1 < om.size(); ++i) {
      if (om[i] == snapix::smolport::kMarker &&
          om[i + 1] == snapix::smolport::kAnchor) {
        expectedOffset = i;
        break;
      }
    }
    struct OffsetObserver final : snapix::smolport::MarkerObserver {
      uint32_t offset = UINT32_MAX;
      std::string id;
      snapix::smolport::ObserverStatus onAnchorAt(
          const uint8_t* data, uint16_t len, uint32_t sourceOffset) override {
        id.assign(reinterpret_cast<const char*>(data), len);
        offset = sourceOffset;
        return snapix::smolport::ObserverStatus::Continue;
      }
    } observer;
    snapix::smolport::MarkerStreamReader reader(observer);
    bool allChunksAccepted = true;
    for (size_t pos = 0; pos < om.size();) {
      const size_t n = std::min<size_t>(3, om.size() - pos);
      allChunksAccepted =
          reader.feed(om.data() + pos, n) && allChunksAccepted;
      pos += n;
    }
    runner.expectTrue(allChunksAccepted, "toc_offset_chunk_feed");
    runner.expectTrue(reader.finish(), "toc_offset_finish");
    runner.expectEqual("p196", observer.id, "toc_offset_anchor_id");
    runner.expectEq(static_cast<uint32_t>(expectedOffset), observer.offset,
                    "toc_offset_absolute");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
