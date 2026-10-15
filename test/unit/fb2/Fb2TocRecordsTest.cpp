// v3.11.12 — fb2toc::RecordReader: one forward pass over the meta.bin TOC
// records written by Fb2::saveMetaCache (title string, i16 section index,
// u32 source offset, u8 depth per item).

#include "test_utils.h"

#include <Fb2TocRecords.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Item {
  std::string title;
  int16_t sectionIndex;
  uint32_t sourceOffset;
  uint8_t depth;
};

template <typename T>
void putPod(std::string& out, const T& v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

// Same byte layout as serialization::writeString + writePod in saveMetaCache.
std::string encode(const std::vector<Item>& items) {
  std::string out;
  for (const auto& item : items) {
    const uint32_t len = static_cast<uint32_t>(item.title.size());
    putPod(out, len);
    out += item.title;
    putPod(out, item.sectionIndex);
    putPod(out, item.sourceOffset);
    putPod(out, item.depth);
  }
  return out;
}

// A reader that hands out at most `chunk` bytes per call, like a file read.
struct ChunkedSource {
  const std::string* data;
  size_t pos;
  size_t chunk;
  int calls;
  int operator()(uint8_t* buf, const size_t len) {
    ++calls;
    const size_t n = std::min({len, chunk, data->size() - pos});
    std::memcpy(buf, data->data() + pos, n);
    pos += n;
    return static_cast<int>(n);
  }
};

std::vector<Item> sampleItems(const size_t count) {
  std::vector<Item> items;
  for (size_t i = 0; i < count; ++i) {
    std::string title = "Глава " + std::to_string(i + 1);
    if (i % 7 == 3) title.clear();  // untitled entries exist in real books
    if (i % 11 == 5) title.append(600, 'x');  // longer than the parse buffer
    items.push_back({title, static_cast<int16_t>(i * 2), static_cast<uint32_t>(1000 + i * 4099),
                     static_cast<uint8_t>(i % 3)});
  }
  return items;
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("Fb2TocRecords");

  const std::vector<Item> items = sampleItems(61);
  const std::string blob = encode(items);

  for (const size_t chunk : {size_t{1}, size_t{3}, size_t{7}, size_t{512}, size_t{100000}}) {
    ChunkedSource source{&blob, 0, chunk, 0};
    auto reader = fb2toc::makeRecordReader(std::ref(source));
    fb2toc::Record record;
    bool allMatch = true;
    for (const auto& expected : items) {
      if (!reader.next(record) || record.title != expected.title || record.sectionIndex != expected.sectionIndex ||
          record.sourceOffset != expected.sourceOffset || record.depth != expected.depth) {
        allMatch = false;
        break;
      }
    }
    runner.expectTrue(allMatch, "all 61 records decode, chunk=" + std::to_string(chunk));
    runner.expectFalse(reader.next(record), "EOF after the last record, chunk=" + std::to_string(chunk));
  }

  {
    // Offsets only: titles skipped, every other field intact.
    ChunkedSource source{&blob, 0, 512, 0};
    auto reader = fb2toc::makeRecordReader(std::ref(source));
    fb2toc::Record record;
    bool ok = true;
    for (const auto& expected : items) {
      ok = ok && reader.next(record, /*withTitle=*/false) && record.title.empty() &&
           record.sourceOffset == expected.sourceOffset && record.sectionIndex == expected.sectionIndex;
    }
    runner.expectTrue(ok, "withTitle=false skips titles and keeps offsets");
    // The point of the reader: a whole TOC in a handful of reads.
    runner.expectTrue(source.calls <= static_cast<int>(blob.size() / 512) + 2,
                      "whole TOC read in ~size/512 block reads");
  }

  {
    // Truncated in the middle of record 10: records 0..9 decode, then false.
    size_t cut = encode(std::vector<Item>(items.begin(), items.begin() + 10)).size() + 6;
    const std::string truncated = blob.substr(0, cut);
    ChunkedSource source{&truncated, 0, 7, 0};
    auto reader = fb2toc::makeRecordReader(std::ref(source));
    fb2toc::Record record;
    int decoded = 0;
    while (reader.next(record)) ++decoded;
    runner.expectEq(10, decoded, "truncated blob: complete records only");
  }

  {
    // A corrupt length (> 64 KB sanity cap) stops the pass instead of
    // allocating it.
    std::string corrupt;
    const uint32_t huge = 0x7FFFFFFFu;
    putPod(corrupt, huge);
    corrupt.append(16, 'z');
    ChunkedSource source{&corrupt, 0, 512, 0};
    auto reader = fb2toc::makeRecordReader(std::ref(source));
    fb2toc::Record record;
    runner.expectFalse(reader.next(record), "oversized title length rejected");
  }

  return runner.allPassed() ? 0 : 1;
}
