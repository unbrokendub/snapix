#include "SmolPngIdatReader.h"

#include "SmolPngFormat.h"

namespace snapix::smolpng {

namespace {

inline uint32_t getBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8)  |
          static_cast<uint32_t>(p[3]);
}

}  // namespace

IdatReader::IdatReader(InputStream& in,
                       const uint32_t firstIdatHeaderOffset)
    : in_(in),
      nextChunkHeaderOffset_(firstIdatHeaderOffset),
      curChunkPayloadCursor_(0),
      curChunkRemaining_(0),
      eof_(false),
      bufLen_(0),
      bufPos_(0) {}

bool IdatReader::seekNextIdat() {
  // Walk chunks from nextChunkHeaderOffset_ forward, looking for IDAT.
  // Stop on IEND or end-of-stream.
  while (nextChunkHeaderOffset_ + kChunkHeaderSize <= in_.length()) {
    uint8_t hdr[kChunkHeaderSize];
    const int got = in_.read(nextChunkHeaderOffset_, hdr,
                              static_cast<size_t>(kChunkHeaderSize));
    if (got != static_cast<int>(kChunkHeaderSize)) return false;

    const uint32_t len     = getBE32(hdr);
    const uint32_t type    = getBE32(hdr + 4);
    const uint32_t payload = nextChunkHeaderOffset_ +
                             static_cast<uint32_t>(kChunkHeaderSize);
    // Advance past payload + CRC for next iteration.
    nextChunkHeaderOffset_ = payload + len + static_cast<uint32_t>(kChunkCrcSize);

    if (type == kIDAT) {
      curChunkPayloadCursor_ = payload;
      curChunkRemaining_     = len;
      return true;
    }
    if (type == kIEND) {
      return false;
    }
    // Ancillary chunk between IDATs — keep scanning.
  }
  return false;
}

bool IdatReader::refillBuffer() {
  // If the current chunk is empty, seek to the next IDAT.
  if (curChunkRemaining_ == 0) {
    if (!seekNextIdat()) return false;
  }
  const uint32_t want = (curChunkRemaining_ < kBufSize)
                          ? curChunkRemaining_
                          : kBufSize;
  const int got = in_.read(curChunkPayloadCursor_, buf_,
                            static_cast<size_t>(want));
  if (got <= 0) return false;
  const uint32_t advance = static_cast<uint32_t>(got);
  bufLen_                  = advance;
  bufPos_                  = 0;
  curChunkPayloadCursor_ += advance;
  curChunkRemaining_     -= advance;
  return true;
}

int IdatReader::nextByte() {
  if (eof_) return -1;
  if (bufPos_ >= bufLen_) {
    if (!refillBuffer()) {
      eof_ = true;
      return -1;
    }
  }
  return buf_[bufPos_++];
}

}  // namespace snapix::smolpng
