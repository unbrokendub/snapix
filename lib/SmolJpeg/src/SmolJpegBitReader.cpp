#include "SmolJpegBitReader.h"

namespace snapix::smoljpeg {

BitReader::BitReader(InputStream& in, const uint32_t startOffset,
                     const uint32_t endOffset)
    : in_(in),
      endOffset_(endOffset),
      bufStart_(startOffset),
      bufPos_(0),
      bufLen_(0),
      bitBuf_(0),
      bitCount_(0),
      marker_(0),
      status_(Status::Ok) {}

bool BitReader::refillBuffer() {
  bufStart_ += bufPos_;
  bufPos_ = 0;
  bufLen_ = 0;
  if (bufStart_ >= endOffset_) return false;

  const uint32_t want = endOffset_ - bufStart_;
  const uint32_t can = (want > sizeof(buf_)) ? sizeof(buf_) : want;
  const int got = in_.read(bufStart_, buf_, can);
  if (got < 0) {
    status_ = Status::InputError;
    return false;
  }
  if (got == 0) return false;
  bufLen_ = static_cast<uint16_t>(got);
  return true;
}

int BitReader::nextDataByte() {
  if (marker_ != 0) return -1;

  // Refill buffer if empty.
  if (bufPos_ >= bufLen_ && !refillBuffer()) {
    // True end of stream — synthesize EOI so the upper layer terminates.
    marker_ = kMarkerEOI;
    return -1;
  }

  const uint8_t b = buf_[bufPos_++];
  if (b != 0xFFu) return b;

  // 0xFF: peel padding bytes (consecutive 0xFFs) and inspect the marker.
  uint8_t m;
  for (;;) {
    if (bufPos_ >= bufLen_ && !refillBuffer()) {
      marker_ = kMarkerEOI;
      return -1;
    }
    m = buf_[bufPos_++];
    if (m != 0xFFu) break;
  }

  if (m == 0x00u) return 0xFF;  // stuffed 0xFF data byte
  marker_ = m;
  return -1;
}

bool BitReader::ensure(const uint32_t n) {
  // Cap at 24 so we never overflow the 32-bit shift register.  Huffman
  // codes are ≤16 bits, so 24 is plenty of headroom.
  const uint32_t target = (n > 24u) ? 24u : n;

  while (bitCount_ < target) {
    const int b = nextDataByte();
    if (b < 0) {
      // Marker hit / EOS.  Pad with 1-bits up to target so Huffman decode
      // can detect "no valid code" cleanly via maxcode walk.
      while (bitCount_ < target) {
        bitBuf_ = (bitBuf_ << 8) | 0xFFu;
        bitCount_ += 8;
      }
      break;
    }
    bitBuf_ = (bitBuf_ << 8) | static_cast<uint32_t>(b);
    bitCount_ += 8;
  }
  return bitCount_ >= n;
}

uint32_t BitReader::readBits(const uint32_t n) {
  if (!ensure(n)) {
    status_ = Status::InvalidJpeg;
    return 0;
  }
  const uint32_t v = (bitBuf_ >> (bitCount_ - n)) & ((1u << n) - 1u);
  bitCount_ -= n;
  if (bitCount_ == 0) {
    bitBuf_ = 0;
  } else {
    bitBuf_ &= (1u << bitCount_) - 1u;
  }
  return v;
}

uint32_t BitReader::peekBits(const uint32_t n) {
  if (!ensure(n)) return 0;
  return (bitBuf_ >> (bitCount_ - n)) & ((1u << n) - 1u);
}

void BitReader::consumeBits(const uint32_t n) {
  if (n >= bitCount_) {
    bitCount_ = 0;
    bitBuf_ = 0;
    return;
  }
  bitCount_ -= n;
  bitBuf_ &= (1u << bitCount_) - 1u;
}

void BitReader::alignToByte() {
  // Drop fractional bits to align on byte boundary.
  const uint32_t frac = bitCount_ & 7u;
  if (frac == 0) return;
  bitCount_ -= frac;
  if (bitCount_ == 0) {
    bitBuf_ = 0;
  } else {
    bitBuf_ &= (1u << bitCount_) - 1u;
  }
}

}  // namespace snapix::smoljpeg
