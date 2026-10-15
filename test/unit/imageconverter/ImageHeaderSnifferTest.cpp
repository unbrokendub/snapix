#include "test_utils.h"

#include <ImageHeaderSniffer.h>

#include <cstdint>
#include <vector>

// ImageHeaderSnifferTest — v3.11.12.  The page-index walk sizes EPUB images
// from the first bytes of the ZIP entry; the sniffer must find the size in a
// forward-only stream however the bytes are chunked, and never invent one.

using snapix::image::ImageHeaderSniffer;
using Result = ImageHeaderSniffer::Result;

namespace {

void be16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x & 0xFF));
}

std::vector<uint8_t> jpeg(uint16_t w, uint16_t h, uint8_t sof, size_t app1Bytes) {
  std::vector<uint8_t> v = {0xFF, 0xD8};
  // APP0 JFIF
  v.insert(v.end(), {0xFF, 0xE0});
  be16(v, 16);
  for (int i = 0; i < 14; ++i) v.push_back('J');
  // Large APP1 (EXIF / embedded thumbnail containing FF D8 FF C0 lookalikes).
  if (app1Bytes > 0) {
    v.insert(v.end(), {0xFF, 0xE1});
    be16(v, static_cast<uint16_t>(app1Bytes + 2));
    for (size_t i = 0; i < app1Bytes; ++i) v.push_back((i % 4 == 0) ? 0xFF : (i % 4 == 1 ? 0xC0 : 0x11));
  }
  // DQT
  v.insert(v.end(), {0xFF, 0xDB});
  be16(v, 67);
  for (int i = 0; i < 65; ++i) v.push_back(1);
  // fill bytes before the SOF marker
  v.insert(v.end(), {0xFF, 0xFF, sof});
  be16(v, 17);
  v.push_back(8);
  be16(v, h);
  be16(v, w);
  for (int i = 0; i < 10; ++i) v.push_back(0);
  v.insert(v.end(), {0xFF, 0xDA, 0x00, 0x08});
  return v;
}

std::vector<uint8_t> png(uint32_t w, uint32_t h) {
  std::vector<uint8_t> v = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
  for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(w >> s));
  for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(h >> s));
  v.insert(v.end(), {8, 0, 0, 0, 0});
  return v;
}

std::vector<uint8_t> bmp(int32_t w, int32_t h) {
  std::vector<uint8_t> v(54, 0);
  v[0] = 'B';
  v[1] = 'M';
  for (int i = 0; i < 4; ++i) v[18 + i] = static_cast<uint8_t>(static_cast<uint32_t>(w) >> (8 * i));
  for (int i = 0; i < 4; ++i) v[22 + i] = static_cast<uint8_t>(static_cast<uint32_t>(h) >> (8 * i));
  return v;
}

Result sniff(const std::vector<uint8_t>& data, size_t chunk, uint16_t& w, uint16_t& h) {
  ImageHeaderSniffer s;
  for (size_t i = 0; i < data.size() && s.result() == Result::NeedMore; i += chunk) {
    const size_t n = std::min(chunk, data.size() - i);
    s.feed(data.data() + i, n);
  }
  w = s.width();
  h = s.height();
  return s.result();
}

}  // namespace

int main() {
  TestUtils::TestRunner runner("ImageHeaderSniffer");

  for (size_t chunk : {static_cast<size_t>(1), static_cast<size_t>(7), static_cast<size_t>(512)}) {
    const std::string c = "_chunk" + std::to_string(chunk);
    uint16_t w = 0, h = 0;

    runner.expectTrue(sniff(jpeg(1200, 1800, 0xC0, 0), chunk, w, h) == Result::Found && w == 1200 && h == 1800,
                      "jpeg_baseline" + c);
    runner.expectTrue(sniff(jpeg(640, 480, 0xC2, 3000), chunk, w, h) == Result::Found && w == 640 && h == 480,
                      "jpeg_progressive_after_big_app1" + c);
    runner.expectTrue(sniff(png(300, 7000), chunk, w, h) == Result::Found && w == 300 && h == 7000, "png" + c);
    runner.expectTrue(sniff(bmp(452, -600), chunk, w, h) == Result::Found && w == 452 && h == 600,
                      "bmp_top_down" + c);
  }

  uint16_t w = 0, h = 0;
  // DHT (C4) is not a SOF; a JPEG whose scan starts before any SOF is invalid.
  {
    std::vector<uint8_t> v = {0xFF, 0xD8, 0xFF, 0xC4, 0x00, 0x04, 0x00, 0x00, 0xFF, 0xDA, 0x00, 0x08};
    runner.expectTrue(sniff(v, 1, w, h) == Result::Invalid, "jpeg_dht_then_sos_invalid");
  }
  {
    std::vector<uint8_t> v = {'G', 'I', 'F', '8', '9', 'a', 10, 0, 10, 0};
    runner.expectTrue(sniff(v, 4, w, h) == Result::Invalid, "gif_unsupported_invalid");
  }
  {
    auto v = jpeg(100, 100, 0xC0, 0);
    v.resize(30);  // truncated before SOF
    runner.expectTrue(sniff(v, 5, w, h) == Result::NeedMore, "jpeg_truncated_needs_more");
  }
  runner.expectTrue(sniff(png(0, 10), 3, w, h) == Result::Invalid, "png_zero_width_invalid");

  // fitImageBox: aspect-preserving, never upscales.
  uint16_t ow = 0, oh = 0;
  snapix::image::fitImageBox(1200, 1800, 480, 800, ow, oh);
  runner.expectTrue(ow == 480 && oh == 720, "fit_down_width_bound");
  snapix::image::fitImageBox(1000, 4000, 480, 800, ow, oh);
  runner.expectTrue(ow == 200 && oh == 800, "fit_down_height_bound");
  snapix::image::fitImageBox(32, 32, 480, 800, ow, oh);
  runner.expectTrue(ow == 32 && oh == 32, "fit_small_kept");

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
