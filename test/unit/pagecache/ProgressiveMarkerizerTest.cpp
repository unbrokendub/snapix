#include "test_utils.h"

#include <HtmlStripper.h>
#include <LittleFS.h>
#include <MarkdownStripper.h>
#include <ProgressiveMarkerizer.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

struct CollectSink final : snapix::smolport::HtmlStripperSink {
  std::vector<uint8_t> data;
  void emit(const uint8_t* bytes, size_t size) override {
    data.insert(data.end(), bytes, bytes + size);
  }
};

template <typename Stripper>
std::string onePass(const std::string& source) {
  CollectSink sink;
  Stripper stripper(sink);
  stripper.feed(reinterpret_cast<const uint8_t*>(source.data()), source.size());
  stripper.finish();
  return std::string(sink.data.begin(), sink.data.end());
}

template <typename Stripper>
std::string progressive(const std::string& source,
                        const std::vector<size_t>& cuts,
                        const std::string& prefix) {
  snapix::pagecache::StatefulMarkerizer<Stripper> markerizer;
  size_t start = 0;
  std::string output;
  for (size_t i = 0; i <= cuts.size(); ++i) {
    const size_t end = i < cuts.size() ? cuts[i] : source.size();
    const std::string path = prefix + std::to_string(i);
    File file = LittleFS.open(path.c_str(), "w");
    markerizer.begin(&file);
    markerizer.feed(reinterpret_cast<const uint8_t*>(source.data() + start),
                    end - start);
    if (end == source.size()) markerizer.finish();
    markerizer.detach();
    file.close();
    output += LittleFS.getWrittenData(path);
    start = end;
  }
  return output;
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("ProgressiveMarkerizer");
  LittleFS.clearFiles();

  {
    const std::string source =
        "<h1>Title &amp; more</h1><style>.drop{}</style>"
        "<p>A <b>bold</b> paragraph.</p>";
    const std::vector<size_t> cuts = {7, 14, 31, 49, 61};
    runner.expectEqual(
        onePass<snapix::smolport::HtmlStripper>(source),
        progressive<snapix::smolport::HtmlStripper>(
            source, cuts, "/progressive/html-"),
        "html_sink_switch_is_byte_identical");
  }

  {
    const std::string source =
        "# Heading\n\nA **bold** paragraph split across a boundary.\n"
        "```\ncode line\n```\nTail.";
    const std::vector<size_t> cuts = {4, 19, 37, 55, 68};
    runner.expectEqual(
        onePass<snapix::markdown::MarkdownStripper>(source),
        progressive<snapix::markdown::MarkdownStripper>(
            source, cuts, "/progressive/markdown-"),
        "markdown_sink_switch_is_byte_identical");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
