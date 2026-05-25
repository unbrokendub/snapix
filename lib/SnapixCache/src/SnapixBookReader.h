#pragma once

// =============================================================================
// SnapixBookReader — random-access reader over the unified .bin format.
//
// Usage:
//   MyFileReader src(file);
//   SnapixBookReader book;
//   if (!book.open(src)) { /* corrupt or wrong version */ }
//   uint32_t pageIdx, chapIdx;
//   if (book.findAnchor("chap1", chapIdx, pageIdx)) { … }
//   uint8_t chunk[4096];
//   int got = book.readChapterBytes(0, 0, chunk, sizeof(chunk));
//
// All reads stream from the underlying BlobReader — the reader keeps
// only the 64-byte header + table sizes in memory.  Per-anchor lookups
// scan the anchor table linearly with FNV-1a hash pre-filter.
//
// Thread-safety: not thread-safe.  Caller serialises access.
// =============================================================================

#include "SnapixBookFormat.h"
#include "SnapixBookIO.h"

#include <cstddef>
#include <cstdint>

namespace snapix::cache {

class SnapixBookReader {
 public:
  SnapixBookReader();

  // Validate and remember the header from `src`.  Returns true on
  // success.  Reasons for failure: short read, bad magic, unsupported
  // version, table offsets out of range, count exceeds defensive caps.
  // After a successful open the SnapixBookReader holds a reference to
  // `src` — the BlobReader must outlive the SnapixBookReader.
  bool open(BlobReader& src);

  // Reset state, drop reference to BlobReader.  Optional; destruction
  // also clears.
  void close();

  // Header field accessors (post-open).
  uint32_t chapterCount() const { return header_.numChapters; }
  uint32_t anchorCount()  const { return header_.numAnchors; }
  uint32_t imageCount()   const { return header_.numImages; }
  uint32_t totalSize()    const { return header_.totalSize; }

  // Chapter length (0 if `idx` out of range).
  uint32_t chapterSize(uint32_t idx) const;

  // Read up to `len` bytes from chapter `idx` starting at `chapterOffset`
  // bytes into the chapter blob.  Returns bytes copied, 0 on out-of-
  // range, -1 on I/O error.
  int readChapterBytes(uint32_t idx, uint32_t chapterOffset, uint8_t* buf,
                       uint32_t len);

  // Image metadata.  Returns true if `idx` is in range and populates
  // outOffset / outLength / outWidth / outHeight.
  bool imageInfo(uint32_t idx, uint32_t& outLength, uint16_t& outWidth,
                 uint16_t& outHeight) const;

  // Read up to `len` bytes from image blob `idx` starting at `imageOffset`.
  int readImageBytes(uint32_t idx, uint32_t imageOffset, uint8_t* buf,
                     uint32_t len);

  // Find anchor by ID (null-terminated).  On match returns true and
  // populates outChapterIdx / outPageIdx.  Linear scan with FNV-1a
  // pre-filter; for typical < 200 anchors per book, completes in < 50us
  // on ESP32-C3.
  bool findAnchor(const char* id, uint32_t& outChapterIdx,
                  uint32_t& outPageIdx);

 private:
  bool readChapterEntry(uint32_t idx, ChapterTableEntry& out) const;
  bool readImageEntry(uint32_t idx, ImageTableEntry& out) const;
  bool readAnchorEntry(uint32_t idx, AnchorTableEntry& out) const;
  bool readAnchorIdString(uint32_t idOffset, char* buf, uint32_t bufLen);

  BlobReader*       src_;
  SnapixBookHeader  header_;
  bool              opened_;
};

}  // namespace snapix::cache
