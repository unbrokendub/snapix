#pragma once

// =============================================================================
// SnapixBook .bin format — single-file per-book unified cache.
//
// Heritage: borrowed from smol-epub's .sbk format and ioma8/cool's compact
// per-book blob.  Both converged on "single random-access file with a
// fixed header + offset tables + concatenated blob region" so the e-reader
// can open the book with a single fd, perform one seek per page load, and
// avoid FAT32 directory-enumeration overhead.
//
// Why this matters on snapix v2:
//   * v2 writes 30-50+ files per book (per-spine .bin + .anchors + images/).
//   * FAT32 directory ops cost 5-50 ms each on slow SD cards — opening a
//     book can take 1-2 seconds purely in `open()` overhead.
//   * Cache invalidation is per-file: a single config bump requires
//     deleting every file in the cache subtree.
//   * Disk fragmentation grows linearly with chapter count.
//
// What this format does NOT do (left to higher layers):
//   * Internal chapter layout (page LUT, glyph metrics, etc.) — chapter
//     blobs are opaque to the cache layer.  The renderer interprets them
//     however it likes (v3: byte-marker streams from Phase 3; v2: legacy
//     page-element serialization).
//   * Compression — blobs are raw bytes.  Future versions can add a
//     `flags` field to chapter entries to indicate compression.
//
// On-disk layout:
//
//   [0..63]    fixed 64-byte header (SnapixBookHeader)
//   [64..]     chapter table  (numChapters × ChapterTableEntry)
//              anchor table   (numAnchors  × AnchorTableEntry)
//              image table    (numImages   × ImageTableEntry)
//              blob region    (chapter blobs, anchor ID strings, images)
//
// All multi-byte integers are little-endian.  ESP32-C3 is native LE so
// reads are direct; host tests use the same encoding.  All offsets are
// absolute file offsets (relative to byte 0 of the .bin), unless noted
// otherwise.
//
// Forward-compat: bump kVersion when changing the layout.  Future
// readers MUST verify the version before parsing.
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace snapix::cache {

// 4-byte magic at file offset 0.  ASCII: "SPXB" = Snapix Book.
constexpr uint8_t kMagic[4] = {'S', 'P', 'X', 'B'};

// Current format version.  Bump on layout change.
constexpr uint16_t kVersion = 1;

// Fixed sizes (bytes) — DO NOT CHANGE without bumping kVersion.
constexpr size_t kHeaderSize        = 64;
constexpr size_t kChapterEntrySize  = 8;
constexpr size_t kAnchorEntrySize   = 16;
constexpr size_t kImageEntrySize    = 12;

// Sentinel for "anchor not found" returned by lookup APIs.
constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

// Maximum byte-counts the reader honours (defensive caps).  These prevent
// runaway parsing of malformed / corrupted files; bump if real books
// exceed them.
constexpr uint32_t kMaxChapters = 4096;
constexpr uint32_t kMaxAnchors  = 65536;
constexpr uint32_t kMaxImages   = 4096;

// ---------------------------------------------------------------------------
// FNV-1a 32-bit hash — used for anchor ID lookups.  Linear scan + hash
// pre-filter beats strcmp-only by ~5× for typical anchor counts.
// Reference: http://isthe.com/chongo/tech/comp/fnv/
// ---------------------------------------------------------------------------
constexpr uint32_t kFnv1aOffsetBasis = 0x811C9DC5u;
constexpr uint32_t kFnv1aPrime       = 0x01000193u;

inline uint32_t fnv1a32(const uint8_t* data, const size_t len) {
  uint32_t h = kFnv1aOffsetBasis;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= kFnv1aPrime;
  }
  return h;
}

inline uint32_t fnv1a32Str(const char* s) {
  uint32_t h = kFnv1aOffsetBasis;
  while (*s != '\0') {
    h ^= static_cast<uint8_t>(*s++);
    h *= kFnv1aPrime;
  }
  return h;
}

// ---------------------------------------------------------------------------
// Header (64 bytes).  Lives at file offset 0.
// ---------------------------------------------------------------------------
struct SnapixBookHeader {
  uint8_t  magic[4];               // "SPXB"
  uint16_t version;                // currently 1
  uint16_t reserved1;              // 0
  uint32_t numChapters;
  uint32_t numAnchors;
  uint32_t numImages;
  uint32_t chapterTableOffset;     // absolute file offset
  uint32_t anchorTableOffset;
  uint32_t imageTableOffset;
  uint32_t blobsOffset;            // start of concatenated blob region
  uint32_t totalSize;              // total file size in bytes
  uint8_t  reserved2[24];          // zero-filled
};
static_assert(sizeof(SnapixBookHeader) == 64,
              "SnapixBookHeader must be exactly 64 bytes");

// ---------------------------------------------------------------------------
// Chapter table entry (8 bytes).
// ---------------------------------------------------------------------------
struct ChapterTableEntry {
  uint32_t offset;    // absolute file offset to chapter blob
  uint32_t length;    // chapter blob size in bytes
};
static_assert(sizeof(ChapterTableEntry) == 8,
              "ChapterTableEntry must be exactly 8 bytes");

// ---------------------------------------------------------------------------
// Anchor table entry (16 bytes).
//
// `idHash` is FNV-1a of the ID string (no null terminator).  Lookups
// scan by hash first, then confirm with a strcmp.  `idOffset` points at
// the start of the null-terminated ID inside the blob region.
// ---------------------------------------------------------------------------
struct AnchorTableEntry {
  uint32_t idHash;
  uint32_t idOffset;
  uint32_t chapterIdx;
  uint32_t pageIdx;
};
static_assert(sizeof(AnchorTableEntry) == 16,
              "AnchorTableEntry must be exactly 16 bytes");

// ---------------------------------------------------------------------------
// Image table entry (12 bytes).
// ---------------------------------------------------------------------------
struct ImageTableEntry {
  uint32_t offset;
  uint32_t length;
  uint16_t width;
  uint16_t height;
};
static_assert(sizeof(ImageTableEntry) == 12,
              "ImageTableEntry must be exactly 12 bytes");

}  // namespace snapix::cache
