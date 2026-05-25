# SnapixCache — unified single .bin per book cache format

Replaces snapix v2's per-section `.bin` + separate `.anchors` + `images/`
tree with one binary file per book, indexed by table-of-contents headers.

## Status (v2.0.88)

**INFRASTRUCTURE COMPLETE, NOT YET INTEGRATED.**  Both readers and writers
ship (buffered + true-streaming), Arduino adapters for SdFat `FsFile&` and
LittleFS `fs::File&` are header-only.  PageCache migration is the next
step — guarded by `SNAPIX_UNIFIED_CACHE` flag (default 0 in `v3_alpha`).

## What's shipped

| Component | File | Purpose |
|---|---|---|
| Format constants | `SnapixBookFormat.h` | Magic, version, table entry sizes, FNV-1a |
| Reader | `SnapixBookReader.h/.cpp` | Random-access via `BlobReader` |
| Buffered writer | `SnapixBookWriter.h/.cpp` | Accumulates blobs in RAM, finalises in one call |
| Streaming writer | `SnapixBookStreamWriter.h/.cpp` | Streams blobs to seekable output, fills tables on finalize |
| Arduino adapters | `SnapixCacheArduinoAdapter.h` | `FsFile` / `fs::File` → `BlobReader`/`BlobSeekableWriter` (header-only, guarded by `__has_include`) |

## File format

```
[0..63]   fixed header (magic "SPXB" + version + counts + table offsets)
[64..]    chapter table     (numChapters × 8 bytes)
          anchor table      (numAnchors  × 16 bytes)
          image table       (numImages   × 12 bytes)
          blob region       (chapter blobs, anchor ID strings, images)
```

All multi-byte ints little-endian.  Anchor lookup uses FNV-1a 32-bit hash
pre-filter + strcmp confirm.  Chapter / image blobs are opaque to this
library — caller's responsibility to interpret.

## Integration sketch (Phase 5b)

```cpp
#if SNAPIX_UNIFIED_CACHE
#include <SnapixBookReader.h>
#include <SnapixBookStreamWriter.h>
#include <SnapixCacheArduinoAdapter.h>
using namespace snapix::cache;

// READ side — open cache via SnapixBookReader instead of per-section.
FsFile cacheFile;
SdMan.openFileForRead(tag, "/Books/.../unified.bin", cacheFile);
FsFileBlobReader rd(cacheFile);
SnapixBookReader book;
if (book.open(rd)) {
  uint8_t buf[4096];
  const int got = book.readChapterBytes(chapterIdx, /*offset=*/0, buf, sizeof(buf));
  // Feed buf to the existing page-blob parser unchanged.
}

// WRITE side — build the file once during cache creation.
FsFile out;
SdMan.openFileForWrite(tag, "/Books/.../unified.bin", out);
FsFileBlobSeekableWriter sink(out);
SnapixBookStreamWriter wr;
wr.begin(sink, numChapters, numAnchors, numImages);
for (auto& chapter : spine) {
  wr.beginChapter();
  // ... per-section serialization writes via wr.chapterAppend(...) ...
  wr.endChapter();
}
for (auto& a : anchors) wr.addAnchor(a.id.c_str(), a.chapterIdx, a.pageIdx);
wr.finalize();
#endif
```

`SnapixBookStreamWriter` is the production-ready writer for embedded use —
peak RAM is `O(tables)` (~50-100 KB for a typical book) instead of
`O(total blob bytes)` (~1-3 MB).

## Footprint

- Reader: ~1 KB code
- Buffered writer: ~3 KB code (uses std::vector for staging)
- Streaming writer: ~1.3 KB code (uses heap-allocated table arrays)
- BSS: 0

## Why FNV-1a for anchors

Linear scan with hash pre-filter beats strcmp-only by ~5× for typical
anchor counts.  No locality benefit (anchors aren't read frequently —
usually once per TOC click).  Hash collisions are checked via strcmp
confirm so correctness is guaranteed.
