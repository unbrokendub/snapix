#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

// v3.11.12 — forward-only parser for the TOC records Fb2::saveMetaCache()
// writes back to back into meta.bin:
//
//   u32 titleLen, titleLen bytes, i16 sectionIndex, u32 sourceOffset, u8 depth
//
// (little-endian, serialization::writeString / writePod).  It pulls the file
// through one small buffer, so a whole TOC costs one open, one seek and a few
// block reads.  The per-item path (seek + skipString seek + small reads per
// entry, or one open per entry in Fb2::getTocItem) cost 1.6 s for the global
// page metrics and 2.4 s for the TOC overlay on a 61-section book on device.
namespace fb2toc {

struct Record {
  std::string title;
  int16_t sectionIndex = -1;
  uint32_t sourceOffset = 0;
  uint8_t depth = 0;
};

constexpr uint32_t kMaxTitleBytes = 65536;  // same sanity cap as serialization::readString

// ReadFn: int(uint8_t* buf, size_t len) → bytes read, <= 0 at EOF / error.
template <typename ReadFn, size_t BufferBytes = 512>
class RecordReader {
 public:
  explicit RecordReader(ReadFn read) : read_(std::move(read)) {}

  // False at EOF or on a corrupt record.  withTitle=false skips the title
  // bytes (out.title is left empty).
  bool next(Record& out, const bool withTitle = true) {
    uint32_t len = 0;
    if (!take(&len, sizeof(len)) || len > kMaxTitleBytes) return false;
    if (withTitle) {
      out.title.resize(len);
      if (len > 0 && !take(&out.title[0], len)) return false;
    } else {
      out.title.clear();
      if (!skip(len)) return false;
    }
    return take(&out.sectionIndex, sizeof(out.sectionIndex)) && take(&out.sourceOffset, sizeof(out.sourceOffset)) &&
           take(&out.depth, sizeof(out.depth));
  }

 private:
  bool fill() {
    pos_ = 0;
    const int n = read_(buf_, BufferBytes);
    len_ = n > 0 ? static_cast<size_t>(n) : 0;
    return len_ > 0;
  }

  bool take(void* dst, size_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    while (n > 0) {
      if (pos_ == len_ && !fill()) return false;
      const size_t c = std::min(n, len_ - pos_);
      std::memcpy(d, buf_ + pos_, c);
      pos_ += c;
      d += c;
      n -= c;
    }
    return true;
  }

  bool skip(size_t n) {
    while (n > 0) {
      if (pos_ == len_ && !fill()) return false;
      const size_t c = std::min(n, len_ - pos_);
      pos_ += c;
      n -= c;
    }
    return true;
  }

  ReadFn read_;
  uint8_t buf_[BufferBytes];
  size_t pos_ = 0;
  size_t len_ = 0;
};

template <typename ReadFn>
RecordReader<ReadFn> makeRecordReader(ReadFn read) {
  return RecordReader<ReadFn>(std::move(read));
}

}  // namespace fb2toc
