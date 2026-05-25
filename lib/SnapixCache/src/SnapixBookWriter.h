#pragma once

// =============================================================================
// SnapixBookWriter — build a SnapixBook .bin file in memory and emit it
// to a BlobWriter.
//
// Usage:
//   SnapixBookWriter w;
//   const uint32_t c0 = w.addChapter(buf0, len0);
//   const uint32_t c1 = w.addChapter(buf1, len1);
//   w.addAnchor("chap1", c0, 0);
//   w.addAnchor("end", c1, 4);
//   const uint32_t img0 = w.addImage(imageData, imageLen, 480, 640);
//   MyFileWriter sink(file);
//   w.finalize(sink);
//
// Storage: this writer buffers ALL chapter / anchor / image bytes in
// memory until `finalize()`.  Memory cost = ~ book size.  Fine for
// host-side build pipelines and for small-to-medium books (< 1 MB) on
// PSRAM-equipped ESP32 boards.  For very large books with constrained
// RAM, a streaming variant lands in Phase 5b.
//
// Thread-safety: not thread-safe.
// =============================================================================

#include "SnapixBookFormat.h"
#include "SnapixBookIO.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace snapix::cache {

class SnapixBookWriter {
 public:
  SnapixBookWriter();

  // Add a chapter blob.  Returns the chapter index (0-based) on success,
  // or kInvalidIndex on overflow / OOM.
  uint32_t addChapter(const uint8_t* data, uint32_t length);

  // Add an image blob with explicit dimensions.  Returns image index.
  uint32_t addImage(const uint8_t* data, uint32_t length, uint16_t width,
                    uint16_t height);

  // Register an anchor pointing at (chapterIdx, pageIdx).  ID is copied
  // into the writer's internal buffer.  Returns true on success.
  bool addAnchor(const char* id, uint32_t chapterIdx, uint32_t pageIdx);

  // Compute the final layout and emit the .bin to `out`.  After this
  // call the writer's internal state is consumed but not auto-reset —
  // call `reset()` if you want to start a new book.
  bool finalize(BlobWriter& out);

  // Clear all accumulated state.
  void reset();

  // Diagnostics (mostly for tests).
  uint32_t chapterCount() const { return static_cast<uint32_t>(chapters_.size()); }
  uint32_t anchorCount()  const { return static_cast<uint32_t>(anchors_.size()); }
  uint32_t imageCount()   const { return static_cast<uint32_t>(images_.size()); }

 private:
  struct PendingChapter {
    std::vector<uint8_t> data;
  };
  struct PendingImage {
    std::vector<uint8_t> data;
    uint16_t width;
    uint16_t height;
  };
  struct PendingAnchor {
    std::string id;
    uint32_t    chapterIdx;
    uint32_t    pageIdx;
  };

  std::vector<PendingChapter> chapters_;
  std::vector<PendingImage>   images_;
  std::vector<PendingAnchor>  anchors_;
};

}  // namespace snapix::cache
