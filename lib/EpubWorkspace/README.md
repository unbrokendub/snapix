# EpubWorkspace — static workspace in BSS for transient parse state

Single fixed-size struct in BSS that absorbs the largest per-chapter
heap allocations of the EPUB render pipeline.  Pattern borrowed from
ioma8/cool ("cool" e-reader).

## Status (v2.0.88)

**INFRASTRUCTURE COMPLETE, NOT YET INTEGRATED.**  The struct is sized,
allocated in BSS, and exposes its API (arena allocator + anchor table +
scratch buffers).  Integration is the next step — guarded by
`SNAPIX_STATIC_WORKSPACE` flag (default 0).

## What's shipped

```cpp
namespace snapix::workspace {
struct EpubRenderWorkspace {
  uint8_t  htmlReadBuf[2048];           // XML parser chunk read buffer
  uint16_t wordWidths[256];             // per-page word width pre-measures
  uint16_t lineBreaks[64];              // per-block line-break indices
  uint8_t  partWordBuf[256];            // split-word stitching scratch
  uint16_t alignStack[8];               // block-style nesting
  uint16_t listStack[4];                // list nesting
  AnchorSlot anchors[256];              // anchor map (id arena offset + page)
  uint8_t  stringArenaBuf[4096];        // bump allocator for small strings

  void resetForChapter();
  uint8_t* arenaAlloc(uint16_t n);
  const char* arenaCopyStr(const char* s);
  bool addAnchor(const char* id, uint16_t pageNum);
  const char* anchorId(uint16_t index) const;
  uint16_t anchorPage(const char* id) const;
};

EpubRenderWorkspace& workspace();   // canonical singleton accessor
}
```

## Footprint

- 8 KB BSS (zero-initialized at boot)
- ~540 B code

## Integration sketch (Phase 4b)

Replace per-chapter `std::vector<WordData>`, `std::vector<size_t>`,
`std::vector<std::pair<std::string, uint16_t>>` (anchors) etc. in
`ChapterHtmlSlimParser` with workspace slots.  Suggested staged
migration:

1. `anchorMap_` → `workspace().addAnchor(...)` / `workspace().anchorPage(...)`
   (biggest heap win — anchor strings often 50-300 bytes each)
2. `partWordBuffer_` (200 B) → `workspace().partWordBuf` (zero net change
   but moves from heap to BSS)
3. `wordWidths` per text block → `workspace().wordWidths` (per page)
4. `lineBreakIndices` per text block → `workspace().lineBreaks`
5. (Future) extend with per-element style stacks

Each migration is roughly 10-30 LOC of search-and-replace in
ChapterHtmlSlimParser.cpp, guarded by `#if SNAPIX_STATIC_WORKSPACE`.

## Hardware testing notes

The workspace is BSS-allocated even when `SNAPIX_STATIC_WORKSPACE=0` —
8 KB of always-pinned RAM until any caller `#include`s the header.  Once
this lib lands in PIO's LDF graph via a real consumer, expect RAM usage
to bump by 8 KB.  v2.0.88's `v3_alpha` env doesn't yet trigger LDF on
this lib (no consumer), so current RAM is unchanged.

## Why bump arena, not a free-list

Per-chapter lifetime is well-defined: allocate during parse, discard at
chapter end.  Free-list overhead (per-allocation header, fragmentation
tracking) is wasted CPU + memory for that pattern.  Bump arena is
O(1) alloc, O(1) reset, zero fragmentation.
