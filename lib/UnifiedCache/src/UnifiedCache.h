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
constexpr uint16_t kFormatVersion = 4;
constexpr size_t kHeaderSize = 16;
constexpr size_t kFrameHeaderSize = 8;
constexpr uint16_t kGlobalKey = 0xFFFF;
// Special flag value applied to a frame's `flags` byte that marks the segment
// as deleted.  Reading code skips tombstoned frames when building the
// directory; writes that supersede a tombstoned key revive the segment.
constexpr uint8_t kFlagTombstone = 0x01;
// Deferred streaming writes publish the frame header before its final payload
// size is known.  The incomplete bit stays set until size patch + flush
// succeeds; loadDirectory truncates at an incomplete frame after a reset.
constexpr uint8_t kFlagIncomplete = 0x02;

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
  bool compact();
  bool compactIfNeeded();

  // Path on LittleFS — exposed for diagnostics + Cleanup.
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  std::vector<DirectoryEntry> directory_;
  size_t fileSize_ = 0;
  bool loaded_ = false;
  bool headerInitialized_ = false;
  // No per-instance mutex: objects are scoped to one caller operation.  Reader
  // resource ownership serialises foreground/background document access.
  bool ensureLoaded();
  bool loadDirectory();
  bool writeFileHeader(File& file);
  bool recoverCompactionSwap();
  void upsertDirectoryEntry(DirectoryEntry entry);
  size_t compactedFileSize() const;
  bool appendFrame(Kind kind, uint16_t key, uint8_t flags,
                    const std::function<bool(File&)>& writePayload, size_t payloadSize);
  // directory_ is sorted by (key, kind) and stores only the latest frame for
  // each pair, so lookup is logarithmic and history does not consume RAM.
  std::vector<DirectoryEntry>::const_iterator findEntry(Kind kind, uint16_t key) const;
};

}  // namespace snapix::unifiedcache
