#pragma once

// =============================================================================
// ImageHeaderSniffer — forward-only, incremental width/height probe for JPEG,
// PNG and BMP byte streams.  No heap, no seeking: feed it whatever chunks a
// ZIP inflater produces and stop reading as soon as it reports Found/Invalid.
//
// v3.11.12 — page layout must not depend on whether an image's BMP has been
// decoded yet.  The streaming MEASURE walk that builds a chapter's page index
// used to see "no BMP → image skipped (height 0)", while the later render (BMP
// decoded in the background) placed the image at full height: the page
// overflowed and the text the index had put after the image was never shown.
// Layout now uses dimensions sniffed from the SOURCE image header, which are
// available before any decode.
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace snapix::image {

class ImageHeaderSniffer {
 public:
  enum class Result : uint8_t { NeedMore, Found, Invalid };

  Result feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len && result_ == Result::NeedMore; ++i) step(data[i]);
    return result_;
  }

  Result result() const { return result_; }
  uint16_t width() const { return width_; }
  uint16_t height() const { return height_; }

 private:
  enum class Format : uint8_t { Unknown, Jpeg, Png, Bmp };
  enum class JpegState : uint8_t { SeekFF, Code, LenHi, LenLo, Skip, Sof };

  Result result_ = Result::NeedMore;
  Format format_ = Format::Unknown;
  uint32_t pos_ = 0;  // bytes seen
  uint8_t head_[26] = {};

  JpegState js_ = JpegState::SeekFF;
  uint8_t code_ = 0;
  uint32_t remaining_ = 0;
  uint8_t sof_[5] = {};
  uint8_t sofLen_ = 0;

  uint16_t width_ = 0;
  uint16_t height_ = 0;

  void found(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || w > 0xFFFFu || h > 0xFFFFu) {
      result_ = Result::Invalid;
      return;
    }
    width_ = static_cast<uint16_t>(w);
    height_ = static_cast<uint16_t>(h);
    result_ = Result::Found;
  }

  static bool isSof(uint8_t m) {
    // SOF0..SOF15 except DHT (C4), JPG (C8) and DAC (CC).
    return m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
  }

  void step(const uint8_t b) {
    if (pos_ < sizeof(head_)) head_[pos_] = b;
    ++pos_;
    if (format_ == Format::Unknown) {
      if (pos_ < 2) return;
      if (head_[0] == 0xFF && head_[1] == 0xD8) {
        format_ = Format::Jpeg;
        js_ = JpegState::SeekFF;
        return;
      }
      if (head_[0] == 'B' && head_[1] == 'M') {
        format_ = Format::Bmp;
        return;
      }
      if (head_[0] == 0x89 && head_[1] == 'P') {
        format_ = Format::Png;
        return;
      }
      result_ = Result::Invalid;
      return;
    }
    switch (format_) {
      case Format::Png:
        // 8-byte signature, IHDR length (4), "IHDR", width BE (4), height BE (4).
        if (pos_ == 16) {
          static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
          for (int k = 0; k < 8; ++k) {
            if (head_[k] != kSig[k]) {
              result_ = Result::Invalid;
              return;
            }
          }
          if (head_[12] != 'I' || head_[13] != 'H' || head_[14] != 'D' || head_[15] != 'R') {
            result_ = Result::Invalid;
          }
        } else if (pos_ == 24) {
          found((static_cast<uint32_t>(head_[16]) << 24) | (static_cast<uint32_t>(head_[17]) << 16) |
                    (static_cast<uint32_t>(head_[18]) << 8) | head_[19],
                (static_cast<uint32_t>(head_[20]) << 24) | (static_cast<uint32_t>(head_[21]) << 16) |
                    (static_cast<uint32_t>(head_[22]) << 8) | head_[23]);
        }
        return;
      case Format::Bmp:
        // BITMAPINFOHEADER: int32 LE width @18, int32 LE height @22 (negative = top-down).
        if (pos_ == 26) {
          int32_t w = static_cast<int32_t>(static_cast<uint32_t>(head_[18]) | (static_cast<uint32_t>(head_[19]) << 8) |
                                           (static_cast<uint32_t>(head_[20]) << 16) |
                                           (static_cast<uint32_t>(head_[21]) << 24));
          int32_t h = static_cast<int32_t>(static_cast<uint32_t>(head_[22]) | (static_cast<uint32_t>(head_[23]) << 8) |
                                           (static_cast<uint32_t>(head_[24]) << 16) |
                                           (static_cast<uint32_t>(head_[25]) << 24));
          if (w < 0) w = -w;
          if (h < 0) h = -h;
          found(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        }
        return;
      case Format::Jpeg:
        stepJpeg(b);
        return;
      case Format::Unknown:
        return;
    }
  }

  void stepJpeg(const uint8_t b) {
    switch (js_) {
      case JpegState::SeekFF:
        if (b == 0xFF) js_ = JpegState::Code;
        return;
      case JpegState::Code:
        if (b == 0xFF) return;  // fill byte
        code_ = b;
        if (b == 0x00 || b == 0x01 || b == 0xD8 || (b >= 0xD0 && b <= 0xD7)) {
          js_ = JpegState::SeekFF;  // stand-alone marker (or stuffed 0xFF00)
        } else if (b == 0xD9 || b == 0xDA) {
          result_ = Result::Invalid;  // EOI / start of scan before any SOF
        } else {
          js_ = JpegState::LenHi;
        }
        return;
      case JpegState::LenHi:
        remaining_ = static_cast<uint32_t>(b) << 8;
        js_ = JpegState::LenLo;
        return;
      case JpegState::LenLo:
        remaining_ |= b;
        if (remaining_ < 2) {
          result_ = Result::Invalid;
          return;
        }
        remaining_ -= 2;
        if (isSof(code_)) {
          if (remaining_ < 5) {
            result_ = Result::Invalid;
            return;
          }
          sofLen_ = 0;
          js_ = JpegState::Sof;
        } else {
          js_ = remaining_ > 0 ? JpegState::Skip : JpegState::SeekFF;
        }
        return;
      case JpegState::Skip:
        if (--remaining_ == 0) js_ = JpegState::SeekFF;
        return;
      case JpegState::Sof:
        // precision, height (BE16), width (BE16)
        sof_[sofLen_++] = b;
        if (sofLen_ == 5) {
          found((static_cast<uint32_t>(sof_[3]) << 8) | sof_[4], (static_cast<uint32_t>(sof_[1]) << 8) | sof_[2]);
        }
        return;
    }
  }
};

// Aspect-preserving fit of (srcW × srcH) into (maxW × maxH), never upscaling.
// Same arithmetic as the FB2 image registration (Fb2.cpp scaledFit) so EPUB and
// FB2 layout boxes are computed identically.
inline void fitImageBox(uint32_t srcW, uint32_t srcH, uint32_t maxW, uint32_t maxH, uint16_t& outW, uint16_t& outH) {
  outW = 0;
  outH = 0;
  if (srcW == 0 || srcH == 0) return;
  if (maxW == 0 || maxH == 0 || (srcW <= maxW && srcH <= maxH)) {
    outW = static_cast<uint16_t>(srcW > 0xFFFFu ? 0xFFFFu : srcW);
    outH = static_cast<uint16_t>(srcH > 0xFFFFu ? 0xFFFFu : srcH);
    return;
  }
  const double sx = static_cast<double>(maxW) / srcW;
  const double sy = static_cast<double>(maxH) / srcH;
  const double scale = sx < sy ? sx : sy;
  uint32_t w = static_cast<uint32_t>(srcW * scale);
  uint32_t h = static_cast<uint32_t>(srcH * scale);
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  outW = static_cast<uint16_t>(w);
  outH = static_cast<uint16_t>(h);
}

}  // namespace snapix::image
