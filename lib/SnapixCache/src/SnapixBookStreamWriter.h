#pragma once

// =============================================================================
// SnapixBookStreamWriter — true-streaming SnapixBook writer.  Memory cost is
// O(tables) instead of O(book size): table entries (chapter/anchor/image
// offsets) live in heap-allocated arrays sized to the caller-declared
// counts, but the chapter/image blob bytes stream straight to the output
// without buffering.
//
// Why a separate writer (vs `SnapixBookWriter`):
//   * The default `SnapixBookWriter` buffers every blob in std::vector
//     until `finalize()`.  Fine for host build pipelines, infeasible on
//     ESP32-C3 with 145 KB heap — a single 50 KB chapter × 30 chapters =
//     1.5 MB peak.
//   * Streaming layout: reserve header + tables → write blobs in any order
//     → seek back to start, fill in header + tables.
//   * Caller must declare numChapters / numAnchors / numImages up-front
//     so we know the table size.  For EPUB conversion that's known after
//     parsing the OPF spine + first-pass anchor sweep.
//
// Usage:
//   FsFileSeekableWriter sink(file);
//   SnapixBookStreamWriter w;
//   if (!w.begin(sink, /*numChapters=*/30, /*numAnchors=*/200,
//                /*numImages=*/15)) { … }
//
//   for (each chapter) {
//     w.beginChapter();
//     while (more bytes) w.chapterAppend(buf, n);
//     w.endChapter();
//   }
//   for (each anchor) w.addAnchor(id, chapterIdx, pageIdx);
//   for (each image)  w.addImage(data, n, w, h);
//   w.finalize();
//
// All operations return bool; first failure stops the cascade.
// =============================================================================

#include "SnapixBookFormat.h"
#include "SnapixBookIO.h"

#include <cstddef>
#include <cstdint>

namespace snapix::cache {

class SnapixBookStreamWriter {
 public:
  SnapixBookStreamWriter();
  ~SnapixBookStreamWriter();
  SnapixBookStreamWriter(const SnapixBookStreamWriter&) = delete;
  SnapixBookStreamWriter& operator=(const SnapixBookStreamWriter&) = delete;

  // Open the writer.  Reserves header (64 B) + tables.  Caller MUST add
  // EXACTLY `numChapters` chapters and `numImages` images before finalize;
  // anchors may be fewer (table is sized to maximum, unused slots are
  // written with zero hashes).  Returns false on allocation failure or
  // seek error.
  bool begin(BlobSeekableWriter& out, uint32_t numChapters,
             uint32_t numAnchors, uint32_t numImages);

  // -------- chapter streaming --------
  // Start a new chapter blob.  The writer records the current output
  // position as the chapter's offset.  Returns true if there's still a
  // table slot available.
  bool beginChapter();

  // Append `len` bytes to the current open chapter.  Many calls allowed.
  bool chapterAppend(const uint8_t* data, uint32_t len);

  // Close the current chapter.  Locks in its byte length.
  bool endChapter();

  // Convenience: add a chapter in one shot when caller has it fully
  // buffered.  Equivalent to beginChapter / chapterAppend / endChapter.
  bool addChapter(const uint8_t* data, uint32_t len);

  // -------- anchor table --------
  bool addAnchor(const char* id, uint32_t chapterIdx, uint32_t pageIdx);

  // -------- image table --------
  bool addImage(const uint8_t* data, uint32_t length, uint16_t width,
                uint16_t height);

  // -------- finalize --------
  // Seek back to offset 0 and emit the header + chapter/anchor/image
  // tables in their pre-reserved slots.  Returns false on seek/write
  // error or if any consistency check fails (e.g. chapters added != declared).
  bool finalize();

  // Diagnostics.
  uint32_t chaptersAdded() const { return numChaptersAdded_; }
  uint32_t anchorsAdded()  const { return numAnchorsAdded_; }
  uint32_t imagesAdded()   const { return numImagesAdded_; }

 private:
  BlobSeekableWriter* out_;
  bool                inProgress_;
  bool                chapterOpen_;

  // Capacities declared at begin().
  uint32_t maxChapters_;
  uint32_t maxAnchors_;
  uint32_t maxImages_;

  // Heap-allocated table-entry arrays — sized to declared maxima.
  ChapterTableEntry* chapterTable_;
  AnchorTableEntry*  anchorTable_;
  ImageTableEntry*   imageTable_;
  uint32_t           numChaptersAdded_;
  uint32_t           numAnchorsAdded_;
  uint32_t           numImagesAdded_;

  // Layout cursors.
  uint32_t chapterTableOffset_;
  uint32_t anchorTableOffset_;
  uint32_t imageTableOffset_;
  uint32_t blobsOffset_;

  // While a chapter is open, this records the offset where it started so
  // endChapter() can compute the length.
  uint32_t curChapterStart_;

  void cleanup();
};

}  // namespace snapix::cache
