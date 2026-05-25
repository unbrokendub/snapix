#include "SnapixBookWriter.h"

#include <cstring>

namespace snapix::cache {

namespace {

inline void putLE16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline void putLE32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

}  // namespace

SnapixBookWriter::SnapixBookWriter() = default;

void SnapixBookWriter::reset() {
  chapters_.clear();
  anchors_.clear();
  images_.clear();
}

uint32_t SnapixBookWriter::addChapter(const uint8_t* data,
                                       const uint32_t length) {
  if (chapters_.size() >= kMaxChapters) return kInvalidIndex;
  PendingChapter c;
  if (length > 0 && data != nullptr) {
    c.data.assign(data, data + length);
  }
  const uint32_t idx = static_cast<uint32_t>(chapters_.size());
  chapters_.push_back(std::move(c));
  return idx;
}

uint32_t SnapixBookWriter::addImage(const uint8_t* data, const uint32_t length,
                                     const uint16_t width,
                                     const uint16_t height) {
  if (images_.size() >= kMaxImages) return kInvalidIndex;
  PendingImage img;
  img.width  = width;
  img.height = height;
  if (length > 0 && data != nullptr) {
    img.data.assign(data, data + length);
  }
  const uint32_t idx = static_cast<uint32_t>(images_.size());
  images_.push_back(std::move(img));
  return idx;
}

bool SnapixBookWriter::addAnchor(const char* id, const uint32_t chapterIdx,
                                  const uint32_t pageIdx) {
  if (id == nullptr || id[0] == '\0') return false;
  if (anchors_.size() >= kMaxAnchors) return false;
  anchors_.push_back({id, chapterIdx, pageIdx});
  return true;
}

bool SnapixBookWriter::finalize(BlobWriter& out) {
  const uint32_t numChapters = static_cast<uint32_t>(chapters_.size());
  const uint32_t numAnchors  = static_cast<uint32_t>(anchors_.size());
  const uint32_t numImages   = static_cast<uint32_t>(images_.size());

  // Layout: header | chapter table | anchor table | image table | blobs
  const uint32_t chapterTableOffset = kHeaderSize;
  const uint32_t anchorTableOffset  = chapterTableOffset + numChapters * kChapterEntrySize;
  const uint32_t imageTableOffset   = anchorTableOffset  + numAnchors  * kAnchorEntrySize;
  const uint32_t blobsOffset        = imageTableOffset   + numImages   * kImageEntrySize;

  // Blob region: chapter blobs, then anchor ID strings (null-terminated),
  // then image blobs.  Compute offsets.
  std::vector<uint32_t> chapterBlobOffsets(numChapters);
  std::vector<uint32_t> anchorIdOffsets(numAnchors);
  std::vector<uint32_t> imageBlobOffsets(numImages);

  uint32_t cursor = blobsOffset;
  for (uint32_t i = 0; i < numChapters; ++i) {
    chapterBlobOffsets[i] = cursor;
    cursor += static_cast<uint32_t>(chapters_[i].data.size());
  }
  for (uint32_t i = 0; i < numAnchors; ++i) {
    anchorIdOffsets[i] = cursor;
    cursor += static_cast<uint32_t>(anchors_[i].id.size() + 1);  // +null
  }
  for (uint32_t i = 0; i < numImages; ++i) {
    imageBlobOffsets[i] = cursor;
    cursor += static_cast<uint32_t>(images_[i].data.size());
  }
  const uint32_t totalSize = cursor;

  // ---- Emit header ----
  uint8_t hdr[kHeaderSize];
  std::memset(hdr, 0, sizeof(hdr));
  std::memcpy(hdr, kMagic, sizeof(kMagic));
  putLE16(hdr + 4,  kVersion);
  // hdr[6..7] reserved1 = 0
  putLE32(hdr + 8,  numChapters);
  putLE32(hdr + 12, numAnchors);
  putLE32(hdr + 16, numImages);
  putLE32(hdr + 20, chapterTableOffset);
  putLE32(hdr + 24, anchorTableOffset);
  putLE32(hdr + 28, imageTableOffset);
  putLE32(hdr + 32, blobsOffset);
  putLE32(hdr + 36, totalSize);
  // hdr[40..63] reserved2 = 0 (already memset)
  if (!out.write(hdr, sizeof(hdr))) return false;

  // ---- Emit chapter table ----
  for (uint32_t i = 0; i < numChapters; ++i) {
    uint8_t buf[kChapterEntrySize];
    putLE32(buf,     chapterBlobOffsets[i]);
    putLE32(buf + 4, static_cast<uint32_t>(chapters_[i].data.size()));
    if (!out.write(buf, sizeof(buf))) return false;
  }

  // ---- Emit anchor table ----
  for (uint32_t i = 0; i < numAnchors; ++i) {
    const auto& a = anchors_[i];
    const uint32_t hash = fnv1a32(reinterpret_cast<const uint8_t*>(a.id.data()),
                                   a.id.size());
    uint8_t buf[kAnchorEntrySize];
    putLE32(buf,      hash);
    putLE32(buf + 4,  anchorIdOffsets[i]);
    putLE32(buf + 8,  a.chapterIdx);
    putLE32(buf + 12, a.pageIdx);
    if (!out.write(buf, sizeof(buf))) return false;
  }

  // ---- Emit image table ----
  for (uint32_t i = 0; i < numImages; ++i) {
    const auto& im = images_[i];
    uint8_t buf[kImageEntrySize];
    putLE32(buf,      imageBlobOffsets[i]);
    putLE32(buf + 4,  static_cast<uint32_t>(im.data.size()));
    putLE16(buf + 8,  im.width);
    putLE16(buf + 10, im.height);
    if (!out.write(buf, sizeof(buf))) return false;
  }

  // ---- Emit chapter blobs ----
  for (const auto& c : chapters_) {
    if (!c.data.empty()) {
      if (!out.write(c.data.data(), static_cast<uint32_t>(c.data.size()))) {
        return false;
      }
    }
  }

  // ---- Emit anchor ID strings (null-terminated) ----
  for (const auto& a : anchors_) {
    if (!out.write(reinterpret_cast<const uint8_t*>(a.id.data()),
                   static_cast<uint32_t>(a.id.size()))) return false;
    const uint8_t nul = 0;
    if (!out.write(&nul, 1)) return false;
  }

  // ---- Emit image blobs ----
  for (const auto& im : images_) {
    if (!im.data.empty()) {
      if (!out.write(im.data.data(), static_cast<uint32_t>(im.data.size()))) {
        return false;
      }
    }
  }

  return true;
}

}  // namespace snapix::cache
