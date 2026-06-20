#pragma once

// v2.0.166 — Phase 5 UnifiedCache: single `streaming.cache` file per book
// replacing the per-spine `markers/<N>.bin` + `markers/<N>.bin.idx` sidecars
// and the global `metrics.bin`.  Sections (page cache) and image BMPs stay
// in separate files for now — they have different lifecycle properties
// (sections grow incrementally + are accessed hot; images live under /img/
// and are shared between formats).
//
// On-disk format (little-endian throughout):
//
//   Header (16 bytes, fixed):
//     magic[4]    "UCAC"
//     version     u16     (kFormatVersion below)
//     flags       u16     (reserved, currently 0)
//     reserved[8]
//
//   Followed by 0+ Frames (variable length):
//     kind        u8      (one of Kind enum below)
//     flags       u8      (bit 0 = tombstone — segment was deleted)
//     key         u16     (spineIndex / sectionIndex; 0xFFFF for global)
//     size        u32     (payload bytes following this header)
//     payload[size]       (segment bytes)
//
// Updates are append-only: writing a new value for an existing (kind, key)
// appends a new frame.  The latest frame for a given key wins.  Old frames
// become garbage; compaction is manual (rewrite-file pass) and not yet
// implemented — for typical reading sessions garbage stays well under 100 KB
// even on long sessions, well within LittleFS budget.
//
// Directory is built in memory on first open by sequentially scanning all
// frames.  Subsequent reads do a single-table lookup → seek → read.

#include <FS.h>
#include <LittleFS.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace snapix::unifiedcache {

// Bumped to WIPE cached MARKERS (+ idx) whenever markerization output changes,
// so the new markers reach books that were ALREADY cached.  EPUB/FB2 markerize is
// skipped on a Markers cache-hit (EpubChapterParser), so a markerization change is
// invisible to existing books unless the UnifiedCache is invalidated here.
// Mismatch wipes the file + re-markerizes on next open (graceful; see load()).
//   v3: v3.10.7 — more basic block tags emit breaks (HTML5 semantic blocks,
//        tables, definition lists) + FB2 now falls through to the common dispatch.
//   v2: v3.10.6 — HTML <h1>-<h6> headings emit a break + centering.
constexpr uint16_t kFormatVersion = 3;
constexpr size_t kHeaderSize = 16;
constexpr size_t kFrameHeaderSize = 8;
constexpr uint16_t kGlobalKey = 0xFFFF;
// Special flag value applied to a frame's `flags` byte that marks the segment
// as deleted.  Reading code skips tombstoned frames when building the
// directory; writes that supersede a tombstoned key revive the segment.
constexpr uint8_t kFlagTombstone = 0x01;

enum class Kind : uint8_t {
  Markers = 1,   // marker stream sidecar (per-spine)
  Idx = 2,       // page boundary index (per-spine, per render config)
  Metrics = 3,   // global page-count cache (key = kGlobalKey)
};

struct DirectoryEntry {
  Kind kind;
  uint16_t key;
  uint32_t offset;  // file offset of the frame's payload (after frame header)
  uint32_t size;    // payload bytes
};

class UnifiedCache {
 public:
  // v2.0.186 — factory returns UnifiedCache by VALUE (was shared_ptr).
  // Drops the ~16 B shared_ptr struct + ~16-32 B control block that
  // every callsite paid as transient heap.  Across the 12 callsites in
  // a typical chapter render that's ~400-600 B of fragmentation
  // avoided (modest but free).  RVO/NRVO elides the move on every
  // compiler we support; the returned instance lives in the caller's
  // automatic storage.
  //
  // Caller pattern changes from arrow to dot:
  //   auto cache = UnifiedCache::shared(path);
  //   cache->segmentSize(...)   →   cache.segmentSize(...)
  // 12 callsites updated (EpubChapterParser, Fb2Parser, ReaderState).
  //
  // Pre-v2.0.186 history (kept for context):
  //   v2.0.173 — factory creates a fresh instance per call (no caching).
  //     v2.0.170/171/172 attempted instance caching (first an unbounded
  //     registry, then LRU-size-1) and broke EPUB reading after FB2
  //     sessions because the cached instance's per-instance state
  //     (especially the std::mutex FreeRTOS semaphore handle) pinned
  //     heap fragments at the wrong moment.  Reverted to per-call
  //     instances; v2.0.186 just drops the shared_ptr indirection on
  //     top of that.
  static UnifiedCache shared(const std::string& bookCachePath);

  // Direct constructor — kept public for unit tests and for callers that
  // explicitly want an isolated instance (e.g., one-shot diagnostics).
  // Production code should use `shared()` for consistency.
  explicit UnifiedCache(std::string bookCachePath);

  // Reads the whole segment into `out`.  Returns false if the segment doesn't
  // exist or on I/O error.  Suitable for small segments (idx, metrics).
  bool readSegment(Kind kind, uint16_t key, std::vector<uint8_t>& out);

  // Get the size of a segment without reading it.  Returns false if missing.
  bool segmentSize(Kind kind, uint16_t key, size_t* outSize);

  // Open a read handle positioned at the start of the segment.  Caller reads
  // up to `*outSize` bytes via `outFile.read()`; reading past the segment's
  // declared size yields bytes from neighbouring frames (caller's
  // responsibility to respect the size).  Returns false if segment missing.
  // Use this for large segments (markers, ~50 KB typical) to avoid loading
  // the whole thing into RAM.
  bool openSegmentReader(Kind kind, uint16_t key, File& outFile, size_t* outSize);

  // Replace (or create) a segment with `data[0..size)`.  Atomically appends a
  // new frame and updates the in-memory directory.  Old frame for the same
  // (kind, key) becomes garbage.  Returns false on I/O failure.
  bool writeSegment(Kind kind, uint16_t key, const uint8_t* data, size_t size);

  // Streaming-write variant for segments the producer doesn't have in a
  // single buffer.  The callback receives a writable `File&` positioned at
  // the payload start; it should write exactly `expectedSize` bytes and
  // return true on success.  If the callback returns false or writes the
  // wrong number of bytes, the frame is rolled back (truncate file).
  bool writeSegmentStreaming(Kind kind, uint16_t key, size_t expectedSize,
                              const std::function<bool(File&)>& writeCb);

  // v2.0.167 — streaming-write variant where the producer doesn't know the
  // payload size up front (e.g. markerize-from-ZIP discovers size only after
  // the HtmlStripper has processed the whole input stream).  The frame
  // header is written with a placeholder size; the callback writes payload
  // bytes via `File&`; on success we compute `payloadSize = endPos -
  // payloadStartPos`, seek back to the frame header, and patch the size
  // field.  On callback failure the frame stays partially-written but
  // `loadDirectory`'s truncated-frame check skips it on next open.
  bool writeSegmentStreamingDeferred(Kind kind, uint16_t key,
                                      const std::function<bool(File&)>& writeCb);

  // Tombstone a segment.  Subsequent reads return false; writes revive the
  // key.  Used for explicit invalidation (e.g. config change).
  bool removeSegment(Kind kind, uint16_t key);

  // True if the cache file exists on disk and has been opened at least once.
  bool isLoaded() const { return loaded_; }

  // Total file size on disk (including garbage from superseded frames).
  size_t fileSize() const { return fileSize_; }

  // Sum of live segment payload sizes (without garbage).
  size_t liveSize() const;

  // Path on LittleFS — exposed for diagnostics + Cleanup.
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  std::vector<DirectoryEntry> directory_;
  size_t fileSize_ = 0;
  bool loaded_ = false;
  bool headerInitialized_ = false;
  // v2.0.173 — per-instance std::mutex removed.  Instances are scope-
  // bound to the caller's shared_ptr and never accessed concurrently
  // (each thread that needs a UnifiedCache calls `shared()` to get its
  // own).  The mutex was added in v2.0.170 to guard the v2.0.170
  // registry against concurrent access; the registry is gone, so the
  // mutex is gone too.  This eliminates ~88 B of FreeRTOS semaphore
  // handle allocation per UnifiedCache lifetime — the smoking gun
  // behind the EPUB-after-FB2 InflateReader 32 KB allocation failure.

  bool ensureOpenForRead();
  bool ensureLoaded();
  bool loadDirectory();
  bool writeFileHeader(File& file);
  bool appendFrame(Kind kind, uint16_t key, uint8_t flags,
                    const std::function<bool(File&)>& writePayload, size_t payloadSize);
  // Returns iterator into directory_ or end() if not found.  Latest entry
  // for (kind, key) wins (vector is in insertion order, so we walk
  // backwards).
  std::vector<DirectoryEntry>::const_iterator findEntry(Kind kind, uint16_t key) const;
};

}  // namespace snapix::unifiedcache
