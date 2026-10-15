#include "test_utils.h"

#include <LittleFS.h>
#include <UnifiedCache.h>

#include <cstdint>
#include <string>
#include <vector>

using snapix::unifiedcache::Kind;
using snapix::unifiedcache::UnifiedCache;

namespace {

std::vector<uint8_t> bytes(const std::string& value) {
  return std::vector<uint8_t>(value.begin(), value.end());
}

bool write(UnifiedCache& cache, Kind kind, uint16_t key,
           const std::string& value) {
  const auto data = bytes(value);
  return cache.writeSegment(kind, key, data.data(), data.size());
}

std::string read(UnifiedCache& cache, Kind kind, uint16_t key) {
  std::vector<uint8_t> data;
  if (!cache.readSegment(kind, key, data)) return {};
  return std::string(data.begin(), data.end());
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("UnifiedCache");
  LittleFS.clearFiles();

  {
    auto cache = UnifiedCache::shared("/cache/book");
    runner.expectTrue(write(cache, Kind::Markers, 1, "old"),
                      "write_initial");
    runner.expectTrue(write(cache, Kind::Idx, 2, "index"),
                      "write_second_key");
    runner.expectTrue(write(cache, Kind::Markers, 1, "new-value"),
                      "overwrite_key");
    runner.expectEq(static_cast<size_t>(14), cache.liveSize(),
                    "live_size_counts_latest_only");
    runner.expectEqual("new-value", read(cache, Kind::Markers, 1),
                       "latest_value_wins");

    const size_t before = cache.fileSize();
    runner.expectTrue(cache.compact(), "forced_compaction_succeeds");
    runner.expectTrue(cache.fileSize() < before, "compaction_shrinks_file");
    runner.expectEqual("new-value", read(cache, Kind::Markers, 1),
                       "value_survives_compaction");
    runner.expectEqual("index", read(cache, Kind::Idx, 2),
                       "second_key_survives_compaction");

    runner.expectTrue(cache.removeSegment(Kind::Idx, 2), "tombstone_key");
    size_t ignored = 0;
    runner.expectFalse(cache.segmentSize(Kind::Idx, 2, &ignored),
                       "tombstone_hides_old_value");
    runner.expectEq(static_cast<size_t>(9), cache.liveSize(),
                    "tombstone_not_live");

    runner.expectTrue(cache.writeSegment(Kind::Metrics, 7, nullptr, 0),
                      "empty_live_segment_written");
    size_t emptySize = 99;
    runner.expectTrue(cache.segmentSize(Kind::Metrics, 7, &emptySize),
                      "empty_live_segment_exists");
    runner.expectEq(static_cast<size_t>(0), emptySize,
                    "empty_live_segment_size");
  }

  {
    auto cache = UnifiedCache::shared("/cache/abort");
    runner.expectTrue(write(cache, Kind::Markers, 0, "stable"),
                      "abort_seed");
    const bool deferred = cache.writeSegmentStreamingDeferred(
        Kind::Markers, 0, [](File& out) {
          const uint8_t partial[] = {'b', 'a', 'd'};
          out.write(partial, sizeof(partial));
          return false;
        });
    runner.expectFalse(deferred, "deferred_failure_reported");
    runner.expectEqual("stable", read(cache, Kind::Markers, 0),
                       "incomplete_tail_self_heals");

    const bool fixedSize = cache.writeSegmentStreaming(
        Kind::Markers, 0, 3, [](File& out) {
          const uint8_t completeButRejected[] = {'b', 'a', 'd'};
          out.write(completeButRejected, sizeof(completeButRejected));
          return false;
        });
    runner.expectFalse(fixedSize, "fixed_stream_failure_reported");
    runner.expectEqual("stable", read(cache, Kind::Markers, 0),
                       "rejected_complete_payload_never_published");
  }

  {
    // v3.11.13 — progressive EPUB chapters store marker chunks under kinds
    // markerChunkKind(k) (0x10 + k) plus a MarkerChunksDone record; a fresh
    // instance (directory rebuilt from disk) and compaction must keep them.
    using snapix::unifiedcache::kMaxMarkerChunks;
    using snapix::unifiedcache::markerChunkKind;
    {
      auto cache = UnifiedCache::shared("/cache/chunks");
      runner.expectTrue(write(cache, Kind::Markers, 3, "whole"), "chunks_seed_whole_frame");
      runner.expectTrue(write(cache, markerChunkKind(0), 3, "c0"), "chunks_write_first");
      runner.expectTrue(write(cache, markerChunkKind(1), 3, "c1"), "chunks_write_second");
      runner.expectTrue(write(cache, markerChunkKind(kMaxMarkerChunks - 1), 3, "last"), "chunks_write_max_kind");
      runner.expectTrue(write(cache, Kind::MarkerChunksDone, 3, "done"), "chunks_write_done_record");
    }
    auto reloaded = UnifiedCache::shared("/cache/chunks");
    runner.expectEqual("c0", read(reloaded, markerChunkKind(0), 3), "chunk_kind_survives_reload");
    runner.expectEqual("c1", read(reloaded, markerChunkKind(1), 3), "chunk_kinds_are_distinct");
    runner.expectEqual("last", read(reloaded, markerChunkKind(kMaxMarkerChunks - 1), 3),
                       "highest_chunk_kind_survives_reload");
    runner.expectEqual("done", read(reloaded, Kind::MarkerChunksDone, 3), "done_record_survives_reload");
    runner.expectEqual("whole", read(reloaded, Kind::Markers, 3), "chunk_kinds_do_not_clobber_markers");
    runner.expectTrue(reloaded.compact(), "chunks_compaction_succeeds");
    runner.expectEqual("c1", read(reloaded, markerChunkKind(1), 3), "chunk_kind_survives_compaction");
  }

  {
    auto cache = UnifiedCache::shared("/cache/rename");
    runner.expectTrue(write(cache, Kind::Markers, 0, "one"),
                      "rename_seed");
    runner.expectTrue(write(cache, Kind::Markers, 0, "two"),
                      "rename_overwrite");
    LittleFS.failNextRename();
    runner.expectFalse(cache.compact(), "rename_failure_reported");
    runner.expectEqual("two", read(cache, Kind::Markers, 0),
                       "rename_failure_keeps_original");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
