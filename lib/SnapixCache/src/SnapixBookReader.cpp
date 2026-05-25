#include "SnapixBookReader.h"

#include <cstring>

namespace snapix::cache {

namespace {

// Little-endian readers.  All multi-byte fields in the format are LE.
inline uint16_t getLE16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t getLE32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool readExact(BlobReader& src, uint32_t offset, uint8_t* buf, uint32_t len) {
  uint32_t got = 0;
  while (got < len) {
    const int n = src.read(offset + got, buf + got, len - got);
    if (n <= 0) return false;
    got += static_cast<uint32_t>(n);
  }
  return true;
}

}  // namespace

SnapixBookReader::SnapixBookReader() : src_(nullptr), header_{}, opened_(false) {}

void SnapixBookReader::close() {
  src_ = nullptr;
  opened_ = false;
  header_ = SnapixBookHeader{};
}

bool SnapixBookReader::open(BlobReader& src) {
  close();
  src_ = &src;

  // Read 64-byte header.
  uint8_t buf[kHeaderSize];
  if (!readExact(src, 0, buf, sizeof(buf))) return false;

  // Magic.
  if (std::memcmp(buf, kMagic, sizeof(kMagic)) != 0) return false;

  // Decode fields.
  header_.magic[0] = buf[0]; header_.magic[1] = buf[1];
  header_.magic[2] = buf[2]; header_.magic[3] = buf[3];
  header_.version            = getLE16(buf + 4);
  header_.reserved1          = getLE16(buf + 6);
  header_.numChapters        = getLE32(buf + 8);
  header_.numAnchors         = getLE32(buf + 12);
  header_.numImages          = getLE32(buf + 16);
  header_.chapterTableOffset = getLE32(buf + 20);
  header_.anchorTableOffset  = getLE32(buf + 24);
  header_.imageTableOffset   = getLE32(buf + 28);
  header_.blobsOffset        = getLE32(buf + 32);
  header_.totalSize          = getLE32(buf + 36);
  std::memcpy(header_.reserved2, buf + 40, sizeof(header_.reserved2));

  if (header_.version != kVersion) return false;
  if (header_.numChapters > kMaxChapters) return false;
  if (header_.numAnchors  > kMaxAnchors)  return false;
  if (header_.numImages   > kMaxImages)   return false;
  if (header_.totalSize   > src.size())   return false;

  // Defensive: every table must fall inside the file.
  const auto inRange = [&](const uint32_t off, const uint32_t bytes) {
    if (bytes == 0) return true;
    if (off < kHeaderSize) return false;
    if (off > header_.totalSize) return false;
    if (off + bytes > header_.totalSize) return false;
    return true;
  };
  if (!inRange(header_.chapterTableOffset,
               header_.numChapters * kChapterEntrySize)) return false;
  if (!inRange(header_.anchorTableOffset,
               header_.numAnchors * kAnchorEntrySize)) return false;
  if (!inRange(header_.imageTableOffset,
               header_.numImages * kImageEntrySize)) return false;
  if (header_.blobsOffset > header_.totalSize) return false;

  opened_ = true;
  return true;
}

bool SnapixBookReader::readChapterEntry(const uint32_t idx,
                                        ChapterTableEntry& out) const {
  if (!opened_ || !src_) return false;
  if (idx >= header_.numChapters) return false;
  const uint32_t off = header_.chapterTableOffset + idx * kChapterEntrySize;
  uint8_t buf[kChapterEntrySize];
  if (!readExact(*src_, off, buf, sizeof(buf))) return false;
  out.offset = getLE32(buf);
  out.length = getLE32(buf + 4);
  return true;
}

bool SnapixBookReader::readImageEntry(const uint32_t idx,
                                      ImageTableEntry& out) const {
  if (!opened_ || !src_) return false;
  if (idx >= header_.numImages) return false;
  const uint32_t off = header_.imageTableOffset + idx * kImageEntrySize;
  uint8_t buf[kImageEntrySize];
  if (!readExact(*src_, off, buf, sizeof(buf))) return false;
  out.offset = getLE32(buf);
  out.length = getLE32(buf + 4);
  out.width  = getLE16(buf + 8);
  out.height = getLE16(buf + 10);
  return true;
}

bool SnapixBookReader::readAnchorEntry(const uint32_t idx,
                                       AnchorTableEntry& out) const {
  if (!opened_ || !src_) return false;
  if (idx >= header_.numAnchors) return false;
  const uint32_t off = header_.anchorTableOffset + idx * kAnchorEntrySize;
  uint8_t buf[kAnchorEntrySize];
  if (!readExact(*src_, off, buf, sizeof(buf))) return false;
  out.idHash     = getLE32(buf);
  out.idOffset   = getLE32(buf + 4);
  out.chapterIdx = getLE32(buf + 8);
  out.pageIdx    = getLE32(buf + 12);
  return true;
}

uint32_t SnapixBookReader::chapterSize(const uint32_t idx) const {
  ChapterTableEntry e;
  if (!readChapterEntry(idx, e)) return 0;
  return e.length;
}

int SnapixBookReader::readChapterBytes(const uint32_t idx,
                                        const uint32_t chapterOffset,
                                        uint8_t* const buf,
                                        const uint32_t len) {
  if (!opened_ || !src_) return -1;
  ChapterTableEntry e;
  if (!readChapterEntry(idx, e)) return 0;
  if (chapterOffset >= e.length) return 0;
  const uint32_t avail = e.length - chapterOffset;
  const uint32_t toRead = (len < avail) ? len : avail;
  return src_->read(e.offset + chapterOffset, buf, toRead);
}

bool SnapixBookReader::imageInfo(const uint32_t idx, uint32_t& outLength,
                                  uint16_t& outWidth,
                                  uint16_t& outHeight) const {
  ImageTableEntry e;
  if (!readImageEntry(idx, e)) return false;
  outLength = e.length;
  outWidth  = e.width;
  outHeight = e.height;
  return true;
}

int SnapixBookReader::readImageBytes(const uint32_t idx,
                                      const uint32_t imageOffset,
                                      uint8_t* const buf,
                                      const uint32_t len) {
  if (!opened_ || !src_) return -1;
  ImageTableEntry e;
  if (!readImageEntry(idx, e)) return 0;
  if (imageOffset >= e.length) return 0;
  const uint32_t avail = e.length - imageOffset;
  const uint32_t toRead = (len < avail) ? len : avail;
  return src_->read(e.offset + imageOffset, buf, toRead);
}

bool SnapixBookReader::readAnchorIdString(const uint32_t idOffset, char* buf,
                                           const uint32_t bufLen) {
  if (!opened_ || !src_) return false;
  if (bufLen == 0) return false;

  // Read up to bufLen bytes from offset; stop at first null.
  uint32_t pos = 0;
  while (pos < bufLen - 1) {
    const uint32_t chunk = (bufLen - 1 - pos > 64) ? 64u : (bufLen - 1 - pos);
    const int got = src_->read(idOffset + pos, reinterpret_cast<uint8_t*>(buf + pos), chunk);
    if (got <= 0) return false;
    for (int i = 0; i < got; ++i) {
      if (buf[pos + i] == '\0') {
        return true;  // null already in buffer
      }
    }
    pos += static_cast<uint32_t>(got);
  }
  buf[bufLen - 1] = '\0';  // truncated, but null-terminate for safety
  return true;
}

bool SnapixBookReader::findAnchor(const char* id, uint32_t& outChapterIdx,
                                   uint32_t& outPageIdx) {
  if (!opened_ || !src_ || id == nullptr || id[0] == '\0') return false;

  const uint32_t targetHash = fnv1a32Str(id);
  char idBuf[256];

  for (uint32_t i = 0; i < header_.numAnchors; ++i) {
    AnchorTableEntry e;
    if (!readAnchorEntry(i, e)) return false;
    if (e.idHash != targetHash) continue;
    // Hash matches — confirm with strcmp.
    if (!readAnchorIdString(e.idOffset, idBuf, sizeof(idBuf))) continue;
    if (std::strcmp(idBuf, id) == 0) {
      outChapterIdx = e.chapterIdx;
      outPageIdx    = e.pageIdx;
      return true;
    }
  }
  return false;
}

}  // namespace snapix::cache
