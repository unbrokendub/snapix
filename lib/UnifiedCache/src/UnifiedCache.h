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
//     flags       u8      (bit 0 = tombstone, bit 1 = incomplete)
//     key         u16     (spineIndex / sectionIndex; 0xFFFF for global)
//     size        u32     (payload bytes following this header)
//     payload[size]       (segment bytes)
//
// Updates are append-only: writing a new value for an existing (kind, key)
// appends a new frame.  The latest frame for a given key wins.  Old frames
// become garbage.  Compaction rewrites the latest live frames once the file
// has enough reclaimable space; a recoverable `.bak` swap protects the old
// file until the compacted replacement has been validated.
//
// Directory is built in memory on first open by sequentially scanning all
// frames.  Subsequent reads do a single-table lookup → seek → read.
//
// v3.11.12 — instances are operation-local, but the UI task and the ReaderAsync
// worker open their own instances on the same file.  Every modification
// (append, self-heal, wipe, compaction, swap recovery) runs under one
// process-wide writer token; a load that cannot take it never modifies the file
// and treats an incomplete tail as another task's frame in flight.  Offsets are
// rescanned after any rename/remove (compaction, heal) by another instance.

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
// v4: standalone Markdown/HTML switched from one whole-file Markers frame to
// consecutive progressive chunks.  Wipe v3 so a legacy key-0 frame is never
// mistaken for progressive chunk 0.
// v5: v3.11.12 — HtmlStripper no longer drops text: a bare '&' / unknown entity
// / stray '<' is literal (pre-fix it swallowed up to the next ';'/'<'/'>' —
// often the rest of the paragraph), <title> is suppressed, self-closing tags
// close what they open, invisible format chars are dropped, full HTML 4 entity
// set, SVG/FB2 <image *:href>.  Markers change for many EPUB/FB2 chapters.
// v6: v3.11.12 — big EPUB chapters (>64 KB source) markerize progressively
// into chunk frames; wipe so already-cached big chapters move to that path
// (a whole Markers frame would keep the slow one-shot path forever).
// v7: v3.11.12 — (a) EPUB markers honour the book's CSS (class / inline-style
// italic, bold, center, indent, display:none); nested italic/bold/center
// sources are counted; the compiled table is cached as Kind::StyleTable.
// (b) TXT and Markdown text goes through the HtmlStripper code-point gate
// (TextHygiene.h): BOM / soft hyphen / ZWSP / bidi / C0 controls dropped, C1
// read as windows-1252.
constexpr uint16_t kFormatVersion = 7;
constexpr size_t kHeaderSize = 16;
constexpr size_t kFrameHeaderSize = 8;
constexpr uint16_t kGlobalKey = 0xFFFF;
// Special flag value applied to a frame's `flags` byte that marks the segment
// as deleted.  Reading code skips tombstoned frames when building the
// directory; writes that supersede a tombstoned key revive the segment.
constexpr uint8_t kFlagTombstone = 0x01;
// Deferred streaming writes publish the frame header before its final payload
// size is known.  The incomplete bit stays set until size patch + flush
// succeeds; loadDirectory truncates at an incomplete frame after a reset, but
// only under the writer token — while another instance writes, it is that
// writer's frame and the scan just stops there.
constexpr uint8_t kFlagIncomplete = 0x02;

enum class Kind : uint8_t {
  Markers = 1,   // marker stream sidecar (per-spine)
  Idx = 2,       // page boundary index (per-spine, per render config)
  Metrics = 3,   // global page-count cache (key = kGlobalKey)
  // v3.11.12 — written once a chunked section's last marker chunk is stored
  // (see markerChunkKind); its presence means the chunk set is complete.
  MarkerChunksDone = 4,
  // v3.11.12 — the book's compiled CSS table (snapix::smolport::CssStyleTable
  // blob, key = kGlobalKey).  Every EPUB chapter markerizes against it.
  StyleTable = 5,
};

// v3.11.12 — a big EPUB chapter is markerized progressively: chunk k of section
// `key` is stored under kind kMarkerChunkKindBase + k (one frame per chunk, so
// each is written atomically and readable while later chunks are still being
// produced).  A section has EITHER a whole Markers frame OR chunk frames.
// Kind values below the base stay reserved for future fixed kinds.
constexpr uint8_t kMarkerChunkKindBase = 0x10;
constexpr int kMaxMarkerChunks = 0x100 - kMarkerChunkKindBase;  // 240
inline Kind markerChunkKind(int chunk) {
  return static_cast<Kind>(kMarkerChunkKindBase + chunk);
}
inline bool isKnownKind(uint8_t kindByte) {
  return (kindByte >= static_cast<uint8_t>(Kind::Markers) &&
          kindByte <= static_cast<uint8_t>(Kind::StyleTable)) ||
         kindByte >= kMarkerChunkKindBase;
}

struct DirectoryEntry {
  Kind kind;
  uint16_t key;
  uint32_t offset;  // file offset of the frame's payload (after frame header)
  uint32_t size;    // payload bytes
  bool tombstone = false;
};

class UnifiedCache {
 public:
  // Factory returns an operation-local value (RVO/NRVO), not a registry-owned
  // instance.  Callers can pass it by reference through related cache steps to
  // reuse one directory scan without retaining heap across operations:
  //   auto cache = UnifiedCache::shared(path);
  //   cache.segmentSize(...);
  static UnifiedCache shared(const std::string& bookCachePath);

  // v3.11.12 — process-wide counter that changes whenever ANY instance writes,
  // removes, renames, compacts or self-heals a cache file.  A caller that keeps
  // an instance across operations (the reader's per-page render) reuses its
  // directory snapshot only while this value is unchanged — rescanning an
  // 800 KB streaming.cache cost ~140 ms on every page turn.
  static uint32_t writeGeneration();

  // Direct constructor — kept public for unit tests and for callers that
  // explicitly want an isolated instance (e.g., one-shot diagnostics).
  // Production code should use `shared()` for consistency.
  explicit UnifiedCache(std::string bookCachePath);

  // Move-only: a copy made inside a write callback would share this
  // instance's hold on the writer token.
  UnifiedCache(const UnifiedCache&) = delete;
  UnifiedCache& operator=(const UnifiedCache&) = delete;
  UnifiedCache(UnifiedCache&&) = default;
  UnifiedCache& operator=(UnifiedCache&&) = default;

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
  // return true on success.  If it fails or writes the wrong byte count, the
  // frame remains incomplete and is removed by the next directory load.
  bool writeSegmentStreaming(Kind kind, uint16_t key, size_t expectedSize,
                              const std::function<bool(File&)>& writeCb);

  // v2.0.167 — streaming-write variant where the producer doesn't know the
  // payload size up front (e.g. markerize-from-ZIP discovers size only after
  // the HtmlStripper has processed the whole input stream).  The frame
  // header is written with a placeholder size; the callback writes payload
  // bytes via `File&`; on success we compute `payloadSize = endPos -
  // payloadStartPos`, seek back to the frame header, and patch the size
  // field.  The frame is published only after payload + size are flushed; on
  // callback failure the incomplete frame is self-healed on the next access.
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

  // Rewrite the append log to only its latest live frames.  The swap is
  // recoverable through a `.bak` file if power is lost between renames.
  // compactIfNeeded() is cheap when thresholds are not met and is called after
  // successful writes; compact() forces a pass (primarily diagnostics/tests).
  // compact() returns false without touching the file while another instance
  // is writing.
  bool compact();
  bool compactIfNeeded();

  // Path on LittleFS — exposed for diagnostics + Cleanup.
  const std::string& path() const { return path_; }

 private:
  // Holds the process-wide writer token (re-entrant per instance); see
  // UnifiedCache.cpp.
  class WriterLock;
  enum class RepairMode : uint8_t { Unclaimed, Repair, Done, ReadOnly };

  std::string path_;
  std::vector<DirectoryEntry> directory_;
  size_t fileSize_ = 0;
  bool loaded_ = false;
  bool headerInitialized_ = false;
  // The last scan stopped at something only the writer-token holder may
  // repair; the next write rescans under the token.
  bool repairDeferred_ = false;
  // The last write failed only because another writer held the token: no
  // reason to reclaim space.
  bool writerContended_ = false;
  uint8_t writerDepth_ = 0;
  // writeGeneration() / layout epoch the directory reflects.
  uint32_t scanGeneration_ = 0;
  uint32_t scanLayout_ = 0;
  // No per-instance mutex: objects are scoped to one caller operation.  Other
  // instances (other tasks) on the same file are coordinated by the writer
  // token + layout epoch, not by reader resource ownership — the detached
  // warm-page render runs while the worker writes.
  bool ensureLoaded();
  bool loadDirectory();
  RepairMode claimRepair(bool* result);
  bool deferRepair(size_t visibleEnd, const char* what);
  bool openPayload(Kind kind, uint16_t key, File& f, uint32_t* outSize);
  bool writeFileHeader(File& file);
  bool recoverCompactionSwap();
  void upsertDirectoryEntry(DirectoryEntry entry);
  size_t compactedFileSize() const;
  bool appendFrame(Kind kind, uint16_t key, uint8_t flags,
                    const std::function<bool(File&)>& writePayload, size_t payloadSize);
  bool appendDeferredFrame(Kind kind, uint16_t key, const std::function<bool(File&)>& writeCb);
  // After a failed write: true when cache eviction freed space (CacheSpaceHook.h).
  bool reclaimSpaceAfterFailedWrite();
  // directory_ is sorted by (key, kind) and stores only the latest frame for
  // each pair, so lookup is logarithmic and history does not consume RAM.
  std::vector<DirectoryEntry>::const_iterator findEntry(Kind kind, uint16_t key) const;
};

}  // namespace snapix::unifiedcache
