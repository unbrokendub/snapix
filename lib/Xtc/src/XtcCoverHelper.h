#pragma once

#include <FS.h>
#include <Xtc/XtcParser.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace xtc {

// v3.11.12 — writes the 1-bit cover BMP of page 0 a strip of rows at a time,
// straight from the page stream.  Neither the 96 KB XTCH page nor the 48 KB
// 1-bit image is ever resident (the old helper malloc'd the whole page and
// failed with ~60-90 KB free while reading):
//   * 1-bit pages are row-major — one step copies them through.
//   * 2-bit pages are column-major (columns right to left, 8 vertical pixels
//     per byte, plane 1 then plane 2): each step streams the page once and
//     keeps only the bytes of its rows; a pixel is black when it is non-white
//     in either plane, like the reader's black & white rendering.
// The BMP goes to "<path>.tmp" and is renamed on completion, so a half-written
// cover is never visible as the cover.  Each step is bounded (one page stream),
// so the UI loop can run it between frames.
class CoverBmpWriter {
 public:
  enum class Status : uint8_t { InProgress, Done, Failed };

  static constexpr size_t kDefaultStripBytes = 16 * 1024;
  static constexpr size_t kMinStripRows = 8;

  CoverBmpWriter() = default;
  ~CoverBmpWriter();
  CoverBmpWriter(const CoverBmpWriter&) = delete;
  CoverBmpWriter& operator=(const CoverBmpWriter&) = delete;

  // Validates page 0 and writes the BMP header.  False (nothing left behind)
  // on invalid dimensions or I/O failure.
  bool begin(XtcParser& parser, const std::string& coverBmpPath);
  // Writes the next strip.  `maxStripBytes` bounds the transient strip buffer
  // of a 2-bit page (rows rounded down to a multiple of 8, at least 8 rows;
  // halved while the heap cannot provide it).
  Status step(XtcParser& parser, size_t maxStripBytes = kDefaultStripBytes);
  // Abandons the job and removes the temporary file.
  void cancel();

  bool active() const { return active_; }
  uint16_t rowsWritten() const { return nextRow_; }
  uint16_t height() const { return height_; }

 private:
  Status step1Bit(XtcParser& parser);
  Status step2Bit(XtcParser& parser, size_t maxStripBytes);
  Status fail(const char* why);
  bool writePadding();

  File out_;
  std::string finalPath_;
  std::string tmpPath_;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
  uint8_t bitDepth_ = 1;
  uint16_t nextRow_ = 0;
  uint32_t dstRowBytes_ = 0;  // (width + 7) / 8
  uint32_t bmpRowBytes_ = 0;  // padded to 4 bytes
  bool active_ = false;
};

// Generate 1-bit BMP cover from page 0 of an XTC/XTCH file in one call (runs
// CoverBmpWriter to completion).  Caller must ensure parent directory exists.
// Returns true on success.
bool generateCoverBmpFromParser(XtcParser& parser, const std::string& coverBmpPath);

}  // namespace xtc
