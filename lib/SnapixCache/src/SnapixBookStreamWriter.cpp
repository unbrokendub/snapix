#include "SnapixBookStreamWriter.h"

#include <cstring>
#include <new>

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

SnapixBookStreamWriter::SnapixBookStreamWriter()
    : out_(nullptr),
      inProgress_(false),
      chapterOpen_(false),
      maxChapters_(0),
      maxAnchors_(0),
      maxImages_(0),
      chapterTable_(nullptr),
      anchorTable_(nullptr),
      imageTable_(nullptr),
      numChaptersAdded_(0),
      numAnchorsAdded_(0),
      numImagesAdded_(0),
      chapterTableOffset_(0),
      anchorTableOffset_(0),
      imageTableOffset_(0),
      blobsOffset_(0),
      curChapterStart_(0) {}

SnapixBookStreamWriter::~SnapixBookStreamWriter() { cleanup(); }

void SnapixBookStreamWriter::cleanup() {
  delete[] chapterTable_; chapterTable_ = nullptr;
  delete[] anchorTable_;  anchorTable_  = nullptr;
  delete[] imageTable_;   imageTable_   = nullptr;
  inProgress_  = false;
  chapterOpen_ = false;
  out_         = nullptr;
}

bool SnapixBookStreamWriter::begin(BlobSeekableWriter& out,
                                    const uint32_t numChapters,
                                    const uint32_t numAnchors,
                                    const uint32_t numImages) {
  if (inProgress_) cleanup();
  if (numChapters > kMaxChapters) return false;
  if (numAnchors  > kMaxAnchors)  return false;
  if (numImages   > kMaxImages)   return false;

  out_         = &out;
  maxChapters_ = numChapters;
  maxAnchors_  = numAnchors;
  maxImages_   = numImages;
  numChaptersAdded_ = 0;
  numAnchorsAdded_  = 0;
  numImagesAdded_   = 0;

  if (numChapters > 0) {
    chapterTable_ = new (std::nothrow) ChapterTableEntry[numChapters]();
    if (!chapterTable_) { cleanup(); return false; }
  }
  if (numAnchors > 0) {
    anchorTable_ = new (std::nothrow) AnchorTableEntry[numAnchors]();
    if (!anchorTable_) { cleanup(); return false; }
  }
  if (numImages > 0) {
    imageTable_ = new (std::nothrow) ImageTableEntry[numImages]();
    if (!imageTable_) { cleanup(); return false; }
  }

  // Compute the fixed layout: header → chapter table → anchor table →
  // image table → blobs.
  chapterTableOffset_ = static_cast<uint32_t>(kHeaderSize);
  anchorTableOffset_  = chapterTableOffset_ + numChapters * kChapterEntrySize;
  imageTableOffset_   = anchorTableOffset_  + numAnchors  * kAnchorEntrySize;
  blobsOffset_        = imageTableOffset_   + numImages   * kImageEntrySize;

  // Seek to the start of the blobs region, leaving zero-filled header +
  // table slots that finalize() will fill in.  The wrapped sink should
  // accept seeking past current EOF — both FsFile.seekSet and fs::File.seek
  // grow the file on subsequent writes.
  if (!out_->seek(blobsOffset_)) { cleanup(); return false; }

  inProgress_ = true;
  return true;
}

bool SnapixBookStreamWriter::beginChapter() {
  if (!inProgress_ || chapterOpen_) return false;
  if (numChaptersAdded_ >= maxChapters_) return false;

  curChapterStart_ = out_->position();
  chapterTable_[numChaptersAdded_].offset = curChapterStart_;
  chapterTable_[numChaptersAdded_].length = 0;  // filled in endChapter
  chapterOpen_ = true;
  return true;
}

bool SnapixBookStreamWriter::chapterAppend(const uint8_t* data,
                                            const uint32_t len) {
  if (!inProgress_ || !chapterOpen_) return false;
  if (len == 0) return true;
  if (data == nullptr) return false;
  return out_->write(data, len);
}

bool SnapixBookStreamWriter::endChapter() {
  if (!inProgress_ || !chapterOpen_) return false;
  const uint32_t end = out_->position();
  chapterTable_[numChaptersAdded_].length = end - curChapterStart_;
  ++numChaptersAdded_;
  chapterOpen_ = false;
  return true;
}

bool SnapixBookStreamWriter::addChapter(const uint8_t* data,
                                         const uint32_t len) {
  return beginChapter() && chapterAppend(data, len) && endChapter();
}

bool SnapixBookStreamWriter::addAnchor(const char* id,
                                        const uint32_t chapterIdx,
                                        const uint32_t pageIdx) {
  if (!inProgress_) return false;
  if (id == nullptr || id[0] == '\0') return false;
  if (numAnchorsAdded_ >= maxAnchors_) return false;

  // Write the null-terminated ID string into the blob region at the
  // current position.
  const uint32_t idOffset = out_->position();
  const size_t   idLen    = std::strlen(id);
  if (!out_->write(reinterpret_cast<const uint8_t*>(id),
                   static_cast<uint32_t>(idLen))) return false;
  const uint8_t nul = 0;
  if (!out_->write(&nul, 1)) return false;

  AnchorTableEntry& e = anchorTable_[numAnchorsAdded_];
  e.idHash     = fnv1a32(reinterpret_cast<const uint8_t*>(id), idLen);
  e.idOffset   = idOffset;
  e.chapterIdx = chapterIdx;
  e.pageIdx    = pageIdx;
  ++numAnchorsAdded_;
  return true;
}

bool SnapixBookStreamWriter::addImage(const uint8_t* data, const uint32_t length,
                                       const uint16_t width,
                                       const uint16_t height) {
  if (!inProgress_) return false;
  if (numImagesAdded_ >= maxImages_) return false;

  ImageTableEntry& e = imageTable_[numImagesAdded_];
  e.offset = out_->position();
  e.length = length;
  e.width  = width;
  e.height = height;
  if (length > 0 && data != nullptr) {
    if (!out_->write(data, length)) return false;
  }
  ++numImagesAdded_;
  return true;
}

bool SnapixBookStreamWriter::finalize() {
  if (!inProgress_) return false;
  if (chapterOpen_) return false;  // unclosed chapter
  // Permit anchorsAdded < maxAnchors (anchors are optional / discoverable).
  // Require chapters and images to match exactly — they're declared
  // up-front because they're indexed by position.
  if (numChaptersAdded_ != maxChapters_) return false;
  if (numImagesAdded_   != maxImages_)   return false;

  const uint32_t totalSize = out_->position();

  // --- Header ---
  if (!out_->seek(0)) return false;
  uint8_t hdr[kHeaderSize];
  std::memset(hdr, 0, sizeof(hdr));
  std::memcpy(hdr, kMagic, sizeof(kMagic));
  putLE16(hdr + 4,  kVersion);
  putLE32(hdr + 8,  maxChapters_);
  putLE32(hdr + 12, numAnchorsAdded_);
  putLE32(hdr + 16, maxImages_);
  putLE32(hdr + 20, chapterTableOffset_);
  putLE32(hdr + 24, anchorTableOffset_);
  putLE32(hdr + 28, imageTableOffset_);
  putLE32(hdr + 32, blobsOffset_);
  putLE32(hdr + 36, totalSize);
  if (!out_->write(hdr, sizeof(hdr))) return false;

  // --- Chapter table ---
  for (uint32_t i = 0; i < maxChapters_; ++i) {
    uint8_t buf[kChapterEntrySize];
    putLE32(buf,     chapterTable_[i].offset);
    putLE32(buf + 4, chapterTable_[i].length);
    if (!out_->write(buf, sizeof(buf))) return false;
  }

  // --- Anchor table ---
  for (uint32_t i = 0; i < numAnchorsAdded_; ++i) {
    uint8_t buf[kAnchorEntrySize];
    putLE32(buf,      anchorTable_[i].idHash);
    putLE32(buf + 4,  anchorTable_[i].idOffset);
    putLE32(buf + 8,  anchorTable_[i].chapterIdx);
    putLE32(buf + 12, anchorTable_[i].pageIdx);
    if (!out_->write(buf, sizeof(buf))) return false;
  }
  // Any reserved-but-unused anchor slots stay zero-filled (placeholders
  // written at begin() time).  Reader's `numAnchors` from the header
  // bounds the scan to numAnchorsAdded_ exactly.

  // --- Image table ---
  for (uint32_t i = 0; i < maxImages_; ++i) {
    uint8_t buf[kImageEntrySize];
    putLE32(buf,      imageTable_[i].offset);
    putLE32(buf + 4,  imageTable_[i].length);
    putLE16(buf + 8,  imageTable_[i].width);
    putLE16(buf + 10, imageTable_[i].height);
    if (!out_->write(buf, sizeof(buf))) return false;
  }

  // Seek back to total size — leaves the file truncatable to exactly
  // totalSize, though we don't issue a truncate here (caller's job if
  // their underlying file system needs it).
  if (!out_->seek(totalSize)) return false;

  cleanup();
  return true;
}

}  // namespace snapix::cache
