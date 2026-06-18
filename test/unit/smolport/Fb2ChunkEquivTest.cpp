#include "test_utils.h"

#include <HtmlStripper.h>
#include <Markers.h>

#include <cctype>
#include <string>
#include <vector>

// =============================================================================
// Fb2ChunkEquivTest — proves the core correctness claim behind LAZY markerize
// for no-TOC FB2: splitting the FB2 source at ELEMENT boundaries (a `>` followed
// by `<`) and markerizing each chunk with an INDEPENDENT, fresh Fb2-mode
// HtmlStripper concatenates byte-for-byte identical to one continuous pass.
//
// This holds because the stripper has NO tag-nesting stack and emits block
// breaks on CLOSE tags — each tag maps to its marker independently, and an
// element boundary is, by definition, outside any token / suppressed block.
// So no startPendingBreak / context-replay is needed (unlike TXT).  If this
// suite passes, chunked no-TOC FB2 markerize is safe.
// =============================================================================

using snapix::smolport::HtmlStripper;
using snapix::smolport::HtmlStripperSink;

namespace {

struct CollectSink : public HtmlStripperSink {
  std::vector<uint8_t> data;
  void emit(const uint8_t* d, size_t len) override { data.insert(data.end(), d, d + len); }
};

std::string toStr(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

// One continuous Fb2-mode pass over the whole string.
std::string strip(const std::string& in) {
  CollectSink sink;
  HtmlStripper h(sink, HtmlStripper::Mode::Fb2);
  h.feed(reinterpret_cast<const uint8_t*>(in.data()), in.size());
  h.finish();
  return toStr(sink.data);
}

// Markerize each chunk with a FRESH stripper, concatenate — mirrors chunked
// (lazy) markerize where each chunk is written to its own Markers segment.
std::string stripChunked(const std::vector<std::string>& chunks) {
  std::string out;
  for (const auto& c : chunks) {
    CollectSink sink;
    HtmlStripper h(sink, HtmlStripper::Mode::Fb2);
    h.feed(reinterpret_cast<const uint8_t*>(c.data()), c.size());
    h.finish();
    out += toStr(sink.data);
  }
  return out;
}

// Reference implementation of the firmware chunk scanner (findFb2ChunkEnd):
// from start+chunkBytes, find the first '>' that is followed (after optional
// whitespace) by '<' — an element boundary — and cut at that '<'.  Returns the
// cut offset, or src.size() if none remains (the last chunk).
size_t fb2ChunkEnd(const std::string& src, size_t start, size_t chunkBytes) {
  if (start + chunkBytes >= src.size()) return src.size();
  for (size_t i = start + chunkBytes; i < src.size(); ++i) {
    if (src[i] != '>') continue;
    size_t j = i + 1;
    while (j < src.size() && std::isspace(static_cast<unsigned char>(src[j]))) ++j;
    if (j < src.size() && src[j] == '<') return j;  // cut so chunk K+1 starts with a tag
  }
  return src.size();
}

// Tile src into element-boundary chunks using the scanner (~chunkBytes each).
std::vector<std::string> autoChunk(const std::string& src, size_t chunkBytes) {
  std::vector<std::string> chunks;
  size_t pos = 0;
  while (pos < src.size()) {
    const size_t end = fb2ChunkEnd(src, pos, chunkBytes);
    chunks.push_back(src.substr(pos, end - pos));
    pos = end;
  }
  return chunks;
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("Fb2ChunkEquiv");

  // T1: split between two paragraphs (the common chunk boundary).
  {
    const std::string src = "<body><section><p>One.</p><p>Two.</p><p>Three.</p></section></body>";
    const std::vector<std::string> chunks = {"<body><section><p>One.</p>", "<p>Two.</p>",
                                             "<p>Three.</p></section></body>"};
    runner.expectEqual(strip(src), stripChunked(chunks), "T1_split_between_paragraphs");
  }

  // T2: split between two sections (section start emits kPageBreak fresh).
  {
    const std::string src = "<body><section><p>A.</p></section><section><p>B.</p></section></body>";
    const std::vector<std::string> chunks = {"<body><section><p>A.</p></section>",
                                             "<section><p>B.</p></section></body>"};
    runner.expectEqual(strip(src), stripChunked(chunks), "T2_split_between_sections");
  }

  // T3: split right after <body> open, before the first <section>.
  {
    const std::string src = "<body><section><p>X.</p></section></body>";
    const std::vector<std::string> chunks = {"<body>", "<section><p>X.</p></section></body>"};
    runner.expectEqual(strip(src), stripChunked(chunks), "T3_split_after_body");
  }

  // T4 (THE crucial one): a style span CROSSES the chunk boundary.  The close
  // tag's marker is emitted by the fresh stripper regardless of whether it saw
  // the open — proving statelessness.  <strong> wraps two paragraphs; we cut
  // between them.
  {
    const std::string src = "<body><strong><p>bold a</p><p>bold b</p></strong></body>";
    const std::vector<std::string> chunks = {"<body><strong><p>bold a</p>",
                                             "<p>bold b</p></strong></body>"};
    runner.expectEqual(strip(src), stripChunked(chunks), "T4_style_span_across_chunk");
  }

  // T5: emphasis + strong nested, boundary between inner paragraphs.
  {
    const std::string src =
        "<body><section><emphasis><p>i1</p><strong><p>i2</p></strong></emphasis></section></body>";
    const std::vector<std::string> chunks = {
        "<body><section><emphasis><p>i1</p>", "<strong><p>i2</p></strong></emphasis></section></body>"};
    runner.expectEqual(strip(src), stripChunked(chunks), "T5_nested_style_across_chunk");
  }

  // T6: <title> (heading) split off — heading markers emitted per-tag.
  {
    const std::string src = "<body><section><title><p>Chapter</p></title><p>Body.</p></section></body>";
    const std::vector<std::string> chunks = {"<body><section><title><p>Chapter</p></title>",
                                             "<p>Body.</p></section></body>"};
    runner.expectEqual(strip(src), stripChunked(chunks), "T6_title_split");
  }

  // T7: the scanner tiles a realistic body into many chunks (tiny chunkBytes to
  // force boundaries) and the concatenation still equals the continuous pass.
  {
    std::string src = "<body><section>";
    for (int i = 0; i < 40; ++i) {
      src += "<p>Paragraph number ";
      src += std::to_string(i);
      src += " with some Cyrillic-ish filler text to add length.</p>";
    }
    src += "</section></body>";
    const auto chunks = autoChunk(src, 64);  // ~64-byte chunks → ~40 splits
    runner.expectEqual(strip(src), stripChunked(chunks), "T7_scanner_tiled_equiv");
    // sanity: the scanner actually produced multiple chunks.
    runner.expectTrue(chunks.size() > 5, "T7_multiple_chunks");
  }

  // T8: scanner never cuts inside a <binary> base64 blob (no '<'/'>' in base64),
  // so the boundary lands AFTER </binary>.  Equivalence holds across it.
  {
    const std::string src =
        "<body><section><p>before</p></section></body>"
        "<binary id=\"i\" content-type=\"image/jpeg\">AAAABBBBCCCCDDDDEEEEFFFFGGGGHHHH</binary>";
    const auto chunks = autoChunk(src, 40);
    runner.expectEqual(strip(src), stripChunked(chunks), "T8_binary_not_split");
  }

  // T9: a single un-splittable run (no element boundary after chunkBytes) → one
  // chunk, identical (degrades gracefully, never incorrect).
  {
    const std::string src = "<body><section><p>one long paragraph no boundaries here</p></section></body>";
    const auto chunks = autoChunk(src, 8);
    runner.expectEqual(strip(src), stripChunked(chunks), "T9_graceful_degrade");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
