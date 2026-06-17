#include "test_utils.h"

#include <ChunkedMarkersReader.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// =============================================================================
// ChunkedMarkersReaderTest — the span/seek logic that presents N chunk segments
// as one logical concatenated stream (the core of lazy/progressive markerize).
// =============================================================================

using snapix::smolport::ChunkedMarkersReader;
using snapix::smolport::MarkerChunkProvider;

namespace {

// In-memory provider: chunks are byte vectors.
struct VectorChunkProvider : public MarkerChunkProvider {
  std::vector<std::vector<uint8_t>> chunks;
  int cur = -1;
  size_t pos = 0;

  int count() const override { return static_cast<int>(chunks.size()); }
  uint32_t size(int chunk) const override { return static_cast<uint32_t>(chunks[chunk].size()); }
  bool open(int chunk) override {
    if (chunk < 0 || chunk >= count()) return false;
    cur = chunk;
    pos = 0;
    return true;
  }
  int read(uint8_t* buf, size_t bufSize) override {
    if (cur < 0) return -1;
    const auto& c = chunks[cur];
    if (pos >= c.size()) return 0;  // chunk EOF
    size_t n = std::min(bufSize, c.size() - pos);
    std::memcpy(buf, c.data() + pos, n);
    pos += n;
    return static_cast<int>(n);
  }
  bool seekLocal(uint32_t offset) override {
    if (cur < 0 || offset > chunks[cur].size()) return false;
    pos = offset;
    return true;
  }
};

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

// Read the whole stream (from current position) with a given buffer size.
std::string drain(ChunkedMarkersReader& r, size_t bufSize) {
  std::string out;
  std::vector<uint8_t> buf(bufSize);
  for (;;) {
    int n = r.read(buf.data(), bufSize);
    if (n <= 0) break;
    out.append(reinterpret_cast<const char*>(buf.data()), n);
  }
  return out;
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("ChunkedMarkersReader");

  // T1: single chunk behaves like a plain segment.
  {
    VectorChunkProvider p;
    p.chunks = {bytes("Hello, world.")};
    ChunkedMarkersReader r(p);
    runner.expectEq(uint32_t(13), r.totalSize(), "T1_total");
    runner.expectEq(1, r.chunkCount(), "T1_count");
    runner.expectEqual("Hello, world.", drain(r, 64), "T1_drain");
  }

  // T2: three chunks concatenate in order (big buffer).
  {
    VectorChunkProvider p;
    p.chunks = {bytes("AAAA"), bytes("BBBB"), bytes("CCCC")};
    ChunkedMarkersReader r(p);
    runner.expectEq(uint32_t(12), r.totalSize(), "T2_total");
    runner.expectEqual("AAAABBBBCCCC", drain(r, 64), "T2_concat");
  }

  // T3: read spans chunk boundaries with a small buffer (3 bytes).
  {
    VectorChunkProvider p;
    p.chunks = {bytes("AAAA"), bytes("BBBB"), bytes("CCCC")};
    ChunkedMarkersReader r(p);
    runner.expectEqual("AAAABBBBCCCC", drain(r, 3), "T3_small_buf_spans");
  }

  // T4: read buffer larger than a chunk still yields chunk-bounded reads but
  // the full concatenation overall.
  {
    VectorChunkProvider p;
    p.chunks = {bytes("AB"), bytes("CDEF"), bytes("G")};
    ChunkedMarkersReader r(p);
    runner.expectEqual("ABCDEFG", drain(r, 100), "T4_big_buf");
  }

  // T5: seekGlobal to a chunk boundary then read the suffix.
  {
    VectorChunkProvider p;
    p.chunks = {bytes("AAAA"), bytes("BBBB"), bytes("CCCC")};
    ChunkedMarkersReader r(p);
    runner.expectTrue(r.seekGlobal(4), "T5_seek_ok");
    runner.expectEqual("BBBBCCCC", drain(r, 64), "T5_suffix");
  }

  // T6: seekGlobal mid-chunk (offset 6 → 'B'+2 in chunk 1).
  {
    VectorChunkProvider p;
    p.chunks = {bytes("AAAA"), bytes("BBBB"), bytes("CCCC")};
    ChunkedMarkersReader r(p);
    runner.expectTrue(r.seekGlobal(6), "T6_seek_ok");
    runner.expectEqual("BBCCCC", drain(r, 3), "T6_midchunk_suffix");
  }

  // T7: seekGlobal to 0 then full drain == concatenation.
  {
    VectorChunkProvider p;
    p.chunks = {bytes("xy"), bytes("z")};
    ChunkedMarkersReader r(p);
    runner.expectTrue(r.seekGlobal(0), "T7_seek0");
    runner.expectEqual("xyz", drain(r, 1), "T7_from0");
  }

  // T8: seekGlobal to totalSize → immediate EOF (empty suffix).
  {
    VectorChunkProvider p;
    p.chunks = {bytes("AAAA"), bytes("BBBB")};
    ChunkedMarkersReader r(p);
    runner.expectTrue(r.seekGlobal(8), "T8_seek_end");
    runner.expectEqual("", drain(r, 64), "T8_eof");
  }

  // T9: seekGlobal past end → false.
  {
    VectorChunkProvider p;
    p.chunks = {bytes("AAAA")};
    ChunkedMarkersReader r(p);
    runner.expectTrue(!r.seekGlobal(5), "T9_past_end_false");
  }

  // T10: an empty chunk in the middle is skipped transparently.
  {
    VectorChunkProvider p;
    p.chunks = {bytes("AA"), bytes(""), bytes("BB")};
    ChunkedMarkersReader r(p);
    runner.expectEqual("AABB", drain(r, 3), "T10_empty_chunk_skipped");
    // seek into chunk 2 across the empty one
    ChunkedMarkersReader r2(p);
    runner.expectTrue(r2.seekGlobal(2), "T10_seek_ok");
    runner.expectEqual("BB", drain(r2, 64), "T10_seek_suffix");
  }

  // T11: zero chunks → totalSize 0, read returns EOF.
  {
    VectorChunkProvider p;
    ChunkedMarkersReader r(p);
    runner.expectEq(uint32_t(0), r.totalSize(), "T11_empty_total");
    runner.expectEqual("", drain(r, 8), "T11_empty_read");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
