#include "XtcCoverHelper.h"

#include <FS.h>          // Arduino base File (v2.0.73 LittleFS migration)
#include <LittleFS.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#define TAG "XTC_COVER"

namespace xtc {

namespace {

constexpr uint32_t kMaxDimension = 2000;
constexpr uint32_t kBmpHeaderBytes = 14 + 40 + 8;  // file header + BITMAPINFOHEADER + 2-colour palette

template <typename T>
bool writePod(File& f, const T& v) {
  return f.write(reinterpret_cast<const uint8_t*>(&v), sizeof(v)) == sizeof(v);
}

}  // namespace

CoverBmpWriter::~CoverBmpWriter() { cancel(); }

bool CoverBmpWriter::begin(XtcParser& parser, const std::string& coverBmpPath) {
  cancel();
  if (parser.getPageCount() == 0) {
    LOG_ERR(TAG, "No pages in XTC file");
    return false;
  }

  PageInfo pageInfo;
  if (!parser.getPageInfo(0, pageInfo)) {
    LOG_ERR(TAG, "Failed to get first page info");
    return false;
  }
  if (pageInfo.width == 0 || pageInfo.height == 0 || pageInfo.width > kMaxDimension ||
      pageInfo.height > kMaxDimension) {
    LOG_ERR(TAG, "Invalid dimensions: %ux%u", pageInfo.width, pageInfo.height);
    return false;
  }

  width_ = pageInfo.width;
  height_ = pageInfo.height;
  bitDepth_ = parser.getBitDepth();
  dstRowBytes_ = (static_cast<uint32_t>(width_) + 7) / 8;
  bmpRowBytes_ = ((static_cast<uint32_t>(width_) + 31) / 32) * 4;
  nextRow_ = 0;
  finalPath_ = coverBmpPath;
  tmpPath_ = coverBmpPath + ".tmp";

  // v2.0.73: cover lives on LittleFS (cachePath is a LittleFS path).
  out_ = LittleFS.open(tmpPath_.c_str(), "w");
  if (!out_) {
    LOG_ERR(TAG, "Failed to create cover BMP file: %s", tmpPath_.c_str());
    return false;
  }

  const uint32_t imageSize = bmpRowBytes_ * height_;
  const uint32_t fileSize = kBmpHeaderBytes + imageSize;
  const uint32_t reserved = 0;
  const uint32_t dataOffset = kBmpHeaderBytes;
  const uint32_t dibHeaderSize = 40;
  const int32_t width = width_;
  const int32_t height = -static_cast<int32_t>(height_);  // negative = top-down
  const uint16_t planes = 1;
  const uint16_t bitsPerPixel = 1;
  const uint32_t compression = 0;
  const int32_t ppm = 2835;
  const uint32_t colors = 2;
  const uint8_t black[4] = {0x00, 0x00, 0x00, 0x00};
  const uint8_t white[4] = {0xFF, 0xFF, 0xFF, 0x00};
  const bool ok = out_.write('B') == 1 && out_.write('M') == 1 && writePod(out_, fileSize) &&
                  writePod(out_, reserved) && writePod(out_, dataOffset) && writePod(out_, dibHeaderSize) &&
                  writePod(out_, width) && writePod(out_, height) && writePod(out_, planes) &&
                  writePod(out_, bitsPerPixel) && writePod(out_, compression) && writePod(out_, imageSize) &&
                  writePod(out_, ppm) && writePod(out_, ppm) && writePod(out_, colors) && writePod(out_, colors) &&
                  out_.write(black, 4) == 4 && out_.write(white, 4) == 4;
  active_ = true;
  if (!ok) {
    fail("header write failed");
    return false;
  }
  return true;
}

CoverBmpWriter::Status CoverBmpWriter::step(XtcParser& parser, const size_t maxStripBytes) {
  if (!active_) return Status::Failed;
  if (nextRow_ < height_) {
    const Status s = bitDepth_ == 2 ? step2Bit(parser, maxStripBytes) : step1Bit(parser);
    if (s != Status::InProgress || nextRow_ < height_) return s;
  }

  out_.close();
  LittleFS.remove(finalPath_.c_str());
  if (!LittleFS.rename(tmpPath_.c_str(), finalPath_.c_str())) {
    return fail("rename failed");
  }
  active_ = false;
  LOG_INF(TAG, "Generated cover BMP: %s", finalPath_.c_str());
  return Status::Done;
}

bool CoverBmpWriter::writePadding() {
  static const uint8_t kZeros[4] = {0, 0, 0, 0};
  const size_t pad = bmpRowBytes_ - dstRowBytes_;
  return pad == 0 || out_.write(kZeros, pad) == pad;
}

CoverBmpWriter::Status CoverBmpWriter::step1Bit(XtcParser& parser) {
  // XTG rows are already 1-bit, MSB first, 0 = black: copy them through in
  // stream order, padding each row to the BMP's 4-byte stride.
  uint32_t rowFill = 0;
  bool writeOk = true;
  const XtcError err = parser.loadPageStreaming(
      0,
      [&](const uint8_t* data, const size_t size, size_t /*offset*/) {
        size_t i = 0;
        while (writeOk && i < size && nextRow_ < height_) {
          const size_t take = std::min<size_t>(size - i, dstRowBytes_ - rowFill);
          writeOk = out_.write(data + i, take) == take;
          i += take;
          rowFill += static_cast<uint32_t>(take);
          if (rowFill == dstRowBytes_) {
            writeOk = writeOk && writePadding();
            rowFill = 0;
            ++nextRow_;
          }
        }
      },
      4096);
  if (err != XtcError::OK) return fail("page stream failed");
  if (!writeOk) return fail("write failed");
  if (nextRow_ < height_) return fail("page shorter than its header");
  return Status::InProgress;
}

CoverBmpWriter::Status CoverBmpWriter::step2Bit(XtcParser& parser, const size_t maxStripBytes) {
  const uint32_t remaining = static_cast<uint32_t>(height_ - nextRow_);
  uint32_t rows = static_cast<uint32_t>(maxStripBytes / dstRowBytes_) / 8 * 8;
  rows = std::max<uint32_t>(rows, kMinStripRows);
  rows = std::min<uint32_t>(rows, (remaining + 7) / 8 * 8);

  uint8_t* strip = nullptr;
  while (strip == nullptr) {
    strip = static_cast<uint8_t*>(malloc(static_cast<size_t>(rows) * dstRowBytes_));
    if (strip != nullptr) break;
    if (rows <= kMinStripRows) return fail("no heap for a strip");
    rows = std::max<uint32_t>(kMinStripRows, rows / 2 / 8 * 8);
  }
  memset(strip, 0xFF, static_cast<size_t>(rows) * dstRowBytes_);  // white

  // nextRow_ is always a multiple of 8, so the strip covers whole column bytes.
  const uint32_t y0 = nextRow_;
  const uint32_t y1 = std::min<uint32_t>(height_, y0 + rows);
  const size_t colBytes = (static_cast<size_t>(height_) + 7) / 8;
  const size_t planeSize = (static_cast<size_t>(width_) * height_ + 7) / 8;
  const size_t byteLo = y0 / 8;
  const size_t byteHi = (y1 + 7) / 8;

  const XtcError err = parser.loadPageStreaming(
      0,
      [&](const uint8_t* data, const size_t size, const size_t offset) {
        for (size_t i = 0; i < size; ++i) {
          const uint8_t byte = data[i];
          if (byte == 0) continue;  // all 8 pixels white in this plane
          size_t p = offset + i;
          if (p >= planeSize) p -= planeSize;
          if (p >= planeSize) continue;
          const size_t byteInCol = p % colBytes;
          if (byteInCol < byteLo || byteInCol >= byteHi) continue;
          const size_t colIndex = p / colBytes;
          if (colIndex >= width_) continue;
          const size_t x = width_ - 1 - colIndex;
          uint8_t* dstByte = strip + x / 8;
          const uint8_t dstMask = static_cast<uint8_t>(~(0x80u >> (x % 8)));
          for (int b = 0; b < 8; ++b) {
            const size_t y = byteInCol * 8 + static_cast<size_t>(b);
            if (y >= y1) break;
            if ((byte >> (7 - b)) & 1) dstByte[(y - y0) * dstRowBytes_] &= dstMask;
          }
        }
      },
      4096);
  if (err != XtcError::OK) {
    free(strip);
    return fail("page stream failed");
  }

  bool writeOk = true;
  for (uint32_t y = y0; writeOk && y < y1; ++y) {
    writeOk = out_.write(strip + static_cast<size_t>(y - y0) * dstRowBytes_, dstRowBytes_) == dstRowBytes_ &&
              writePadding();
  }
  free(strip);
  if (!writeOk) return fail("write failed");
  nextRow_ = static_cast<uint16_t>(y1);
  return Status::InProgress;
}

CoverBmpWriter::Status CoverBmpWriter::fail(const char* why) {
  LOG_ERR(TAG, "Cover BMP %s: %s", finalPath_.c_str(), why);
  cancel();
  return Status::Failed;
}

void CoverBmpWriter::cancel() {
  if (!active_) return;
  active_ = false;
  out_.close();
  LittleFS.remove(tmpPath_.c_str());
}

bool generateCoverBmpFromParser(XtcParser& parser, const std::string& coverBmpPath) {
  CoverBmpWriter writer;
  if (!writer.begin(parser, coverBmpPath)) return false;
  for (;;) {
    const CoverBmpWriter::Status s = writer.step(parser);
    if (s == CoverBmpWriter::Status::Done) return true;
    if (s == CoverBmpWriter::Status::Failed) return false;
  }
}

}  // namespace xtc
