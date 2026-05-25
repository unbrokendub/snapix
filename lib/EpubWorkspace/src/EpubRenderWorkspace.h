#pragma once

// =============================================================================
// EpubRenderWorkspace — single fixed-size struct in BSS that absorbs the
// largest TRANSIENT per-chapter allocations of the EPUB render pipeline.
//
// Heritage: pattern borrowed from ioma8/cool ("cool" e-reader) which uses
// a single static workspace to side-step heap fragmentation entirely on
// ESP32-C3-class targets.  TrustyReader and smol-epub independently
// converged on similar static-scratch designs.
//
// Why this matters on snapix:
//   * v2.x heap is ~145 KB on Xteink X4.  Cumulative transient allocations
//     during one chapter parse + page-layout pass approach 20-30 KB,
//     producing real fragmentation pressure that occasionally aborts
//     decoding with MIN_FREE_HEAP < 8 KB checks.
//   * Moving the hottest scratch buffers to BSS gives a flat RAM ceiling,
//     returns the equivalent RAM to the genuinely dynamic heap (image
//     decode, mp3 buffers, etc.), and removes a chronic fragmentation
//     producer.
//
// Phase 4b (v2.0.90) scope:
//   * `anchors[]`     — chapter anchor map slots (id-arena-offset + pageNum)
//   * `stringArenaBuf` — bump allocator for anchor IDs (4 KB).
//
// Future phases (4c) may add the previously-prototyped scratch buffers
// (HTML chunk read buffer, per-page word/line widths, alignment & list
// nesting stacks, partial-word stitch buffer) ONCE THEY ARE INTEGRATED.
// Carrying them in BSS before integration burns budget for no benefit —
// see v2.0.90's hardware-bring-up regression where the unused fields
// pushed the heap below the `MIN_LAYOUT_FREE_HEAP` watermark on
// image-heavy Calibre chapters.
//
// Lifecycle:
//   * Single zero-init at boot (BSS).
//   * `resetForChapter()` called at chapter parse start.
//   * No teardown needed at chapter end — next reset wipes counters.
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace snapix::workspace {

// ---------------------------------------------------------------------------
// Sizing constants.  Tuned for Xteink X4 (480 × 800 viewport, ~145 KB heap).
// ---------------------------------------------------------------------------
constexpr size_t kAnchorSlotsCount   = 256;   // 1 KB of (u16, u16) slots
// 4 KB arena.  ~150 long Calibre-style `_idGenObjectAttribute-N` IDs at
// ~26 bytes each fit; most non-Calibre chapters use <40 anchors of <16 B
// (well under capacity).  Arena overflow during emit falls back to the
// heap-vector path so no anchor is lost — see ChapterHtmlSlimParser.cpp.
constexpr size_t kStringArenaBytes   = 4096;

// ---------------------------------------------------------------------------
// Workspace struct.  Lives in BSS as a single static instance — see
// `workspace()` below for the canonical accessor.  Members are zero-init.
// ---------------------------------------------------------------------------
struct EpubRenderWorkspace {
  // ----- anchor map -----
  // Anchor IDs live in `stringArenaBuf` (null-terminated); `slots[i].idArenaOffset`
  // points to the first byte of the ID.  pageNum 0xFFFF == unassigned.
  struct AnchorSlot {
    uint16_t idArenaOffset;
    uint16_t pageNum;
  };
  AnchorSlot anchors[kAnchorSlotsCount];
  uint16_t   anchorCount;

  // ----- string arena (bump allocator) -----
  uint8_t  stringArenaBuf[kStringArenaBytes];
  uint16_t arenaPos;

  // -------------------------------------------------------------------------
  // Lifecycle.
  // -------------------------------------------------------------------------
  // Reset all dynamic counters (arena, anchorCount).  Does NOT zero the
  // backing buffers — bytes are reused as-needed and any stale bytes are
  // overwritten by subsequent writes.
  void resetForChapter();

  // -------------------------------------------------------------------------
  // String arena helpers — bump allocator inside `stringArenaBuf`.
  // -------------------------------------------------------------------------
  // Allocate `n` bytes from the arena.  Returns nullptr on OOM.
  // Caller is responsible for managing the lifetime within the current
  // chapter — `resetForChapter()` invalidates all arena pointers.
  uint8_t* arenaAlloc(uint16_t n);

  // Copy a null-terminated string into the arena.  Returns pointer to the
  // copied null-terminated string (still pointing into arena), or nullptr
  // on OOM.  Convenience wrapper over arenaAlloc.
  const char* arenaCopyStr(const char* s);

  // Arena bytes used / remaining.
  uint16_t arenaUsed()     const { return arenaPos; }
  uint16_t arenaCapacity() const { return kStringArenaBytes; }
  uint16_t arenaFree()     const { return kStringArenaBytes - arenaPos; }

  // -------------------------------------------------------------------------
  // Anchor map helpers.
  // -------------------------------------------------------------------------
  // Register an anchor with given ID and page number.  ID is copied into
  // the arena.  Returns true on success, false on either anchor-slot
  // overflow or arena overflow.
  bool addAnchor(const char* id, uint16_t pageNum);

  // Read back anchor ID by index (returns null-terminated string).  Index
  // must be < anchorCount; otherwise returns nullptr.
  const char* anchorId(uint16_t index) const;

  // Look up an anchor by ID.  Returns pageNum on match, kAnchorNotFound
  // otherwise.  Linear scan — fine for ≤ kAnchorSlotsCount entries.
  static constexpr uint16_t kAnchorNotFound = 0xFFFF;
  uint16_t anchorPage(const char* id) const;
};

// ---------------------------------------------------------------------------
// Canonical accessor for the single BSS-allocated instance.
// ---------------------------------------------------------------------------
EpubRenderWorkspace& workspace();

// ---------------------------------------------------------------------------
// Compile-time budget guard.  v2.0.90 ships only the anchor-map subset.
// If a future phase grows the struct past 8 KB, bump intentionally — this
// ceiling is deliberately tight to remind us that BSS reservations
// directly subtract from layout-time heap headroom (the
// `MIN_LAYOUT_FREE_HEAP` watermark in ChapterHtmlSlimParser).
// ---------------------------------------------------------------------------
static_assert(sizeof(EpubRenderWorkspace) <= 8 * 1024,
              "EpubRenderWorkspace exceeds 8 KB BSS budget; bump the limit "
              "intentionally if this is a planned growth.");

}  // namespace snapix::workspace
