#include "test_utils.h"

#include <CacheSpaceHook.h>
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

std::string readFresh(const std::string& book, Kind kind, uint16_t key) {
  auto cache = UnifiedCache::shared(book);
  return read(cache, kind, key);
}

std::string readViaHandle(UnifiedCache& cache, Kind kind, uint16_t key) {
  File f;
  size_t size = 0;
  if (!cache.openSegmentReader(kind, key, f, &size)) return {};
  std::string out(size, '\0');
  const int got = size == 0 ? 0 : f.read(reinterpret_cast<uint8_t*>(&out[0]), size);
  f.close();
  return got == static_cast<int>(size) ? out : std::string();
}

bool writeRaw(File& out, const std::string& value) {
  return out.write(reinterpret_cast<const uint8_t*>(value.data()), value.size()) == value.size();
}

// A fresh instance scans the file to its end: true when no garbage tail is
// left behind (the scan would stop early and self-heal it otherwise).
bool scansClean(const std::string& book) {
  auto cache = UnifiedCache::shared(book);
  size_t ignored = 0;
  (void)cache.segmentSize(Kind::Metrics, 0xFFFE, &ignored);
  return cache.isLoaded() &&
         cache.fileSize() == LittleFS.getWrittenData(book + "/streaming.cache").size();
}

// v3.11.12 — instances are operation-local, but the UI task (detached warm-page
// render, status-bar metrics, idx save) and the ReaderAsync worker open their
// own instances on the SAME streaming.cache.  The tests below drive the other
// task's instance from inside a writer's payload callback, i.e. while the
// writer's frame is still flagged incomplete.  `out.flush()` commits that
// state the way littlefs does mid-write (the publish flush rewrites the frame).
int gReclaimCalls = 0;
bool gReclaimFrees = false;
std::string gReclaimPath;

bool countingReclaim(const char* path) {
  ++gReclaimCalls;
  gReclaimPath = path ? path : "";
  return gReclaimFrees;
}

// v3.11.12 — a write that fails on a full partition asks cache eviction for
// room (CacheSpaceHook.h); a replayable write retries once when it got some.
void testSpaceReclaim(TestUtils::TestRunner& runner) {
  cache_fs::setWriteFailureHandler(&countingReclaim);
  const std::string book = "/cache/space";
  {
    auto cache = UnifiedCache::shared(book);
    runner.expectTrue(write(cache, Kind::Idx, 1, "seed"), "space_seed");
    gReclaimCalls = 0;
    gReclaimFrees = true;
    LittleFS.failNextWrites(1);
    runner.expectTrue(write(cache, Kind::Idx, 2, "after-reclaim"), "space_write_retried_after_reclaim");
    runner.expectEq(1, gReclaimCalls, "space_reclaim_called_once");
    runner.expectEqual(book + "/streaming.cache", gReclaimPath, "space_reclaim_names_cache_file");
  }
  runner.expectEqual("after-reclaim", readFresh(book, Kind::Idx, 2), "space_retried_frame_persisted");
  runner.expectEqual("seed", readFresh(book, Kind::Idx, 1), "space_older_frame_intact");
  runner.expectTrue(scansClean(book), "space_retry_leaves_no_garbage_tail");
  {
    auto cache = UnifiedCache::shared(book);
    gReclaimCalls = 0;
    gReclaimFrees = false;
    LittleFS.failNextWrites(1);
    runner.expectFalse(write(cache, Kind::Idx, 3, "lost"), "space_write_fails_when_nothing_freed");
    runner.expectEq(1, gReclaimCalls, "space_no_retry_without_room");
  }
  runner.expectEqual("", readFresh(book, Kind::Idx, 3), "space_failed_frame_invisible");
  {
    // A streamed payload was consumed by the failed attempt: reclaim for the
    // caller's replay, but do not run the callback again.
    auto cache = UnifiedCache::shared(book);
    gReclaimCalls = 0;
    gReclaimFrees = true;
    int callbackRuns = 0;
    LittleFS.failNextWrites(1);
    const bool ok = cache.writeSegmentStreamingDeferred(Kind::Markers, 4, [&callbackRuns](File& out) {
      ++callbackRuns;
      return writeRaw(out, "markers");
    });
    runner.expectFalse(ok, "space_deferred_write_reports_failure");
    runner.expectEq(1, gReclaimCalls, "space_deferred_write_reclaims");
    runner.expectEq(0, callbackRuns, "space_deferred_callback_not_replayed");
    runner.expectTrue(cache.writeSegmentStreamingDeferred(Kind::Markers, 4,
                                                          [](File& out) { return writeRaw(out, "markers"); }),
                      "space_deferred_caller_retry_succeeds");
  }
  runner.expectEqual("markers", readFresh(book, Kind::Markers, 4), "space_deferred_retry_persisted");
  cache_fs::setWriteFailureHandler(nullptr);
}

void testCrossInstanceRaces(TestUtils::TestRunner& runner) {
  {
    const std::string book = "/cache/race-deferred";
    {
      auto seed = UnifiedCache::shared(book);
      runner.expectTrue(write(seed, Kind::Markers, 0, "stable"), "race_deferred_seed");
    }
    std::string seenStable;
    bool newFrameVisible = true;
    auto writer = UnifiedCache::shared(book);
    const bool ok = writer.writeSegmentStreamingDeferred(Kind::Markers, 1, [&](File& out) {
      if (!writeRaw(out, "part1-")) return false;
      out.flush();
      auto reader = UnifiedCache::shared(book);
      seenStable = read(reader, Kind::Markers, 0);
      size_t ignored = 0;
      newFrameVisible = reader.segmentSize(Kind::Markers, 1, &ignored);
      return writeRaw(out, "part2");
    });
    runner.expectTrue(ok, "race_deferred_writer_succeeds");
    runner.expectEqual("stable", seenStable, "race_deferred_reader_sees_published_frames");
    runner.expectFalse(newFrameVisible, "race_deferred_in_flight_frame_invisible");
    runner.expectEqual("part1-part2", readFresh(book, Kind::Markers, 1),
                       "race_deferred_frame_survives_concurrent_load");
    runner.expectEqual("stable", readFresh(book, Kind::Markers, 0), "race_deferred_old_frame_intact");
    runner.expectTrue(scansClean(book), "race_deferred_no_garbage_tail");
    runner.expectEqual("part1-part2", read(writer, Kind::Markers, 1), "race_deferred_writer_view_matches_disk");
  }

  {
    // Same race through appendFrame (fixed-size streaming write, also idx /
    // metrics writeSegment): the header carries the final size + incomplete bit.
    const std::string book = "/cache/race-append";
    {
      auto seed = UnifiedCache::shared(book);
      runner.expectTrue(write(seed, Kind::Idx, 3, "index"), "race_append_seed");
    }
    std::string seen;
    auto writer = UnifiedCache::shared(book);
    const bool ok = writer.writeSegmentStreaming(snapix::unifiedcache::markerChunkKind(2), 3, 10, [&](File& out) {
      if (!writeRaw(out, "chunk")) return false;
      out.flush();
      seen = readFresh(book, Kind::Idx, 3);
      return writeRaw(out, "-two!");
    });
    runner.expectTrue(ok, "race_append_writer_succeeds");
    runner.expectEqual("index", seen, "race_append_reader_sees_published_frames");
    runner.expectEqual("chunk-two!", readFresh(book, snapix::unifiedcache::markerChunkKind(2), 3),
                       "race_append_frame_survives_concurrent_load");
    runner.expectTrue(scansClean(book), "race_append_no_garbage_tail");
  }

  {
    // First write into a brand-new file: until its first flush the file exists
    // but is empty.  A concurrent load must not delete it as "too small".
    const std::string book = "/cache/race-fresh-file";
    std::string seen = "unset";
    auto writer = UnifiedCache::shared(book);
    const bool ok = writer.writeSegmentStreamingDeferred(Kind::Markers, 0, [&](File& out) {
      if (!writeRaw(out, "first-")) return false;
      seen = readFresh(book, Kind::Markers, 0);
      return writeRaw(out, "chapter");
    });
    runner.expectTrue(ok, "race_fresh_writer_succeeds");
    runner.expectEqual("", seen, "race_fresh_reader_sees_nothing_yet");
    runner.expectEqual("first-chapter", readFresh(book, Kind::Markers, 0), "race_fresh_file_not_deleted");
    runner.expectTrue(scansClean(book), "race_fresh_no_garbage_tail");
  }

  {
    // A second writer (UI idx/metrics save) while the worker's frame is in
    // flight must not append behind/over it or heal it away; it gives up.
    const std::string book = "/cache/race-two-writers";
    {
      auto seed = UnifiedCache::shared(book);
      runner.expectTrue(write(seed, Kind::Markers, 0, "stable"), "race_writers_seed");
    }
    bool secondWrote = true;
    auto writer = UnifiedCache::shared(book);
    const bool ok = writer.writeSegmentStreamingDeferred(Kind::Markers, 1, [&](File& out) {
      if (!writeRaw(out, "worker-")) return false;
      out.flush();
      auto other = UnifiedCache::shared(book);
      const std::string idx = "ui-idx";
      secondWrote = other.writeSegment(Kind::Idx, 1, reinterpret_cast<const uint8_t*>(idx.data()), idx.size());
      return writeRaw(out, "frame");
    });
    runner.expectTrue(ok, "race_writers_first_succeeds");
    runner.expectFalse(secondWrote, "race_writers_second_backs_off");
    runner.expectEqual("worker-frame", readFresh(book, Kind::Markers, 1), "race_writers_first_frame_intact");
    runner.expectEqual("stable", readFresh(book, Kind::Markers, 0), "race_writers_old_frame_intact");
    runner.expectTrue(scansClean(book), "race_writers_no_garbage_tail");
    runner.expectTrue(write(writer, Kind::Idx, 1, "later"), "race_writers_later_write_succeeds");
    runner.expectEqual("later", readFresh(book, Kind::Idx, 1), "race_writers_later_write_visible");
  }

  {
    // A failed writer leaves a real incomplete tail; with no writer active the
    // next load (any instance) still self-heals it.
    const std::string book = "/cache/race-real-tail";
    {
      auto seed = UnifiedCache::shared(book);
      runner.expectTrue(write(seed, Kind::Markers, 0, "stable"), "real_tail_seed");
      const bool deferred = seed.writeSegmentStreamingDeferred(Kind::Markers, 0, [](File& out) {
        writeRaw(out, "torn");
        out.flush();
        return false;
      });
      runner.expectFalse(deferred, "real_tail_write_fails");
    }
    const size_t tornSize = LittleFS.getWrittenData(book + "/streaming.cache").size();
    runner.expectEqual("stable", readFresh(book, Kind::Markers, 0), "real_tail_old_value_wins");
    runner.expectTrue(LittleFS.getWrittenData(book + "/streaming.cache").size() < tornSize,
                      "real_tail_truncated_by_heal");
    runner.expectTrue(scansClean(book), "real_tail_healed_clean");
    {
      auto cache = UnifiedCache::shared(book);
      runner.expectTrue(write(cache, Kind::Idx, 0, "after"), "real_tail_append_after_heal");
    }
    runner.expectEqual("after", readFresh(book, Kind::Idx, 0), "real_tail_append_reachable");
  }

  {
    // Compaction by one instance moves every frame; another instance that
    // scanned before must not read the new file at its old offsets.
    const std::string book = "/cache/race-stale-offsets";
    {
      auto seed = UnifiedCache::shared(book);
      runner.expectTrue(write(seed, Kind::Markers, 1, "aaaaaaaaaaaaaaaa"), "stale_seed_old");
      runner.expectTrue(write(seed, Kind::Markers, 1, "bbbb"), "stale_seed_new");
      runner.expectTrue(write(seed, Kind::Markers, 2, "second-value"), "stale_seed_second");
    }
    auto reader = UnifiedCache::shared(book);
    size_t size = 0;
    runner.expectTrue(reader.segmentSize(Kind::Markers, 2, &size), "stale_reader_scans_first");
    {
      auto compactor = UnifiedCache::shared(book);
      runner.expectTrue(compactor.compact(), "stale_other_instance_compacts");
    }
    runner.expectEqual("second-value", read(reader, Kind::Markers, 2), "stale_offsets_revalidated_read");
    runner.expectEqual("bbbb", readViaHandle(reader, Kind::Markers, 1), "stale_offsets_revalidated_handle");
  }

  {
    // Compaction from an instance whose snapshot predates another instance's
    // append must not drop that newer frame.
    const std::string book = "/cache/race-stale-compact";
    {
      auto seed = UnifiedCache::shared(book);
      runner.expectTrue(write(seed, Kind::Markers, 1, "old-old-old"), "stale_compact_seed");
      runner.expectTrue(write(seed, Kind::Markers, 1, "new"), "stale_compact_seed_new");
    }
    auto stale = UnifiedCache::shared(book);
    size_t size = 0;
    runner.expectTrue(stale.segmentSize(Kind::Markers, 1, &size), "stale_compact_scans_first");
    {
      auto other = UnifiedCache::shared(book);
      runner.expectTrue(write(other, Kind::Idx, 1, "late-frame"), "stale_compact_other_appends");
    }
    runner.expectTrue(stale.compact(), "stale_compact_runs");
    runner.expectEqual("late-frame", readFresh(book, Kind::Idx, 1), "stale_compact_keeps_newer_frame");
    runner.expectEqual("new", readFresh(book, Kind::Markers, 1), "stale_compact_keeps_latest_value");
  }

  {
    // An instance that scanned before the file existed must not recreate
    // (truncate) it after another instance wrote the first frames.
    const std::string book = "/cache/race-stale-create";
    auto early = UnifiedCache::shared(book);
    size_t size = 0;
    runner.expectFalse(early.segmentSize(Kind::Markers, 0, &size), "stale_create_scans_missing_file");
    {
      auto other = UnifiedCache::shared(book);
      runner.expectTrue(write(other, Kind::Markers, 0, "chapter-zero"), "stale_create_other_writes");
    }
    runner.expectTrue(write(early, Kind::Idx, 0, "idx"), "stale_create_early_writes");
    runner.expectEqual("chapter-zero", readFresh(book, Kind::Markers, 0), "stale_create_keeps_other_frame");
    runner.expectEqual("idx", readFresh(book, Kind::Idx, 0), "stale_create_keeps_own_frame");
  }

  {
    // Plain reads must not bump the write generation: the reader's per-page
    // render reuses one instance while it is unchanged.
    const std::string book = "/cache/race-generation";
    {
      auto seed = UnifiedCache::shared(book);
      runner.expectTrue(write(seed, Kind::Markers, 0, "text"), "generation_seed");
    }
    const uint32_t before = UnifiedCache::writeGeneration();
    runner.expectEqual("text", readFresh(book, Kind::Markers, 0), "generation_read");
    runner.expectEq(before, UnifiedCache::writeGeneration(), "generation_unchanged_by_reads");
    {
      auto cache = UnifiedCache::shared(book);
      runner.expectTrue(write(cache, Kind::Idx, 0, "i"), "generation_write");
    }
    runner.expectTrue(UnifiedCache::writeGeneration() != before, "generation_bumped_by_write");
  }
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
    // v3.11.12 — progressive EPUB chapters store marker chunks under kinds
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

  testSpaceReclaim(runner);
  testCrossInstanceRaces(runner);

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
