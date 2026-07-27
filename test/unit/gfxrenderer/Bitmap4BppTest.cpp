#include <Bitmap.h>
#include <SdFat.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "test_utils.h"

// Bitmap.cpp depends on these display-tuning hooks.  Keep the host test
// deterministic and focused on indexed-pixel unpacking rather than the
// separately-tested e-ink contrast/dither policy.
int adjustPixel(const int gray) { return gray; }
uint8_t quantize(const int gray, const int, const int) { return gray < 128 ? 0 : 3; }

namespace {

void appendLe16(std::string& data, const uint16_t value) {
  data.push_back(static_cast<char>(value & 0xFF));
  data.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void appendLe32(std::string& data, const uint32_t value) {
  data.push_back(static_cast<char>(value & 0xFF));
  data.push_back(static_cast<char>((value >> 8) & 0xFF));
  data.push_back(static_cast<char>((value >> 16) & 0xFF));
  data.push_back(static_cast<char>((value >> 24) & 0xFF));
}

std::string make4BppBmp(const bool declarePaletteSize = true) {
  constexpr uint32_t width = 5;
  constexpr uint32_t height = 1;
  constexpr uint32_t paletteEntries = 16;
  constexpr uint32_t pixelOffset = 14 + 40 + paletteEntries * 4;
  constexpr uint32_t rowBytes = 4;  // 5 nibbles rounded to a 4-byte BMP row.
  constexpr uint32_t fileSize = pixelOffset + rowBytes;

  std::string bmp;
  bmp.reserve(fileSize);

  // BITMAPFILEHEADER
  bmp.append("BM", 2);
  appendLe32(bmp, fileSize);
  appendLe16(bmp, 0);
  appendLe16(bmp, 0);
  appendLe32(bmp, pixelOffset);

  // BITMAPINFOHEADER
  appendLe32(bmp, 40);
  appendLe32(bmp, width);
  appendLe32(bmp, height);
  appendLe16(bmp, 1);
  appendLe16(bmp, 4);
  appendLe32(bmp, 0);  // BI_RGB
  appendLe32(bmp, rowBytes);
  appendLe32(bmp, 2835);
  appendLe32(bmp, 2835);
  appendLe32(bmp, declarePaletteSize ? paletteEntries : 0);
  appendLe32(bmp, paletteEntries);

  // 16-level grayscale BGRA palette.
  for (uint32_t i = 0; i < paletteEntries; ++i) {
    const char gray = static_cast<char>(i * 17);
    bmp.push_back(gray);
    bmp.push_back(gray);
    bmp.push_back(gray);
    bmp.push_back(0);
  }

  // Five pixels: black, white, black, white, white. High nibble first.
  // The last low nibble and fourth row byte are padding.
  bmp.push_back(0x0F);
  bmp.push_back(0x0F);
  bmp.push_back(static_cast<char>(0xF0));
  bmp.push_back(0);

  return bmp;
}

void expectDecodedRow(TestUtils::TestRunner& runner, Bitmap& bitmap, const std::string& label) {
  uint8_t output[2] = {};
  uint8_t sourceRow[4] = {};
  runner.expectEq(static_cast<int>(BmpReaderError::Ok), static_cast<int>(bitmap.readRow(output, sourceRow, 0)),
                  label + ": readRow succeeds");
  runner.expectEq(static_cast<uint8_t>(0x33), output[0], label + ": high/low nibbles decode");
  runner.expectEq(static_cast<uint8_t>(0xC0), output[1], label + ": odd-width final nibble decodes");
}

std::string readBinaryFile(const char* path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}  // namespace

int main(const int argc, char** argv) {
  TestUtils::TestRunner runner("Bitmap 4bpp");

  FsFile file;
  file.setBuffer(make4BppBmp());
  Bitmap bitmap(file);

  runner.expectEq(static_cast<int>(BmpReaderError::Ok), static_cast<int>(bitmap.parseHeaders()),
                  "parseHeaders accepts uncompressed 4bpp BMP");
  runner.expectEq(4, bitmap.getBpp(), "parsed bit depth is 4bpp");
  runner.expectEq(5, bitmap.getWidth(), "parsed width");
  runner.expectEq(1, bitmap.getHeight(), "parsed height");

  expectDecodedRow(runner, bitmap, "streaming parse");

  FsFile implicitPaletteFile;
  implicitPaletteFile.setBuffer(make4BppBmp(false));
  Bitmap implicitPaletteBitmap(implicitPaletteFile);
  runner.expectEq(static_cast<int>(BmpReaderError::Ok), static_cast<int>(implicitPaletteBitmap.parseHeaders()),
                  "biClrUsed=0 defaults to the full 16-entry palette");
  expectDecodedRow(runner, implicitPaletteBitmap, "implicit palette parse");

  FsFile loadedFile;
  loadedFile.setBuffer(make4BppBmp());
  Bitmap loadedBitmap(loadedFile);
  runner.expectEq(static_cast<int>(BmpReaderError::Ok), static_cast<int>(loadedBitmap.parseAndLoadAll()),
                  "parseAndLoadAll accepts 4bpp BMP");
  expectDecodedRow(runner, loadedBitmap, "owned preloaded parse");

  const std::string borrowedData = make4BppBmp();
  FsFile unusedFile;
  Bitmap borrowedBitmap(unusedFile);
  runner.expectEq(static_cast<int>(BmpReaderError::Ok),
                  static_cast<int>(borrowedBitmap.parseFromBorrowedBuffer(
                      reinterpret_cast<const uint8_t*>(borrowedData.data()), borrowedData.size())),
                  "parseFromBorrowedBuffer accepts 4bpp BMP");
  expectDecodedRow(runner, borrowedBitmap, "borrowed preloaded parse");

  // Optional developer integration check: pass a real converter output path.
  // CI remains self-contained by exercising the synthetic BMP above.
  if (argc > 1) {
    const std::string convertedData = readBinaryFile(argv[1]);
    runner.expectFalse(convertedData.empty(), "converter output can be read");

    FsFile convertedFile;
    convertedFile.setBuffer(convertedData);
    Bitmap convertedBitmap(convertedFile);
    runner.expectEq(static_cast<int>(BmpReaderError::Ok), static_cast<int>(convertedBitmap.parseHeaders()),
                    "converter output parses");
    runner.expectEq(4, convertedBitmap.getBpp(), "converter output is 4bpp");
    runner.expectEq(480, convertedBitmap.getWidth(), "converter output width is 480");
    runner.expectEq(800, convertedBitmap.getHeight(), "converter output height is 800");

    std::vector<uint8_t> output((convertedBitmap.getWidth() + 3) / 4);
    std::vector<uint8_t> sourceRow(convertedBitmap.getRowBytes());
    bool allRowsDecoded = true;
    for (int y = 0; y < convertedBitmap.getHeight(); ++y) {
      if (convertedBitmap.readRow(output.data(), sourceRow.data(), y) != BmpReaderError::Ok) {
        allRowsDecoded = false;
        break;
      }
    }
    runner.expectTrue(allRowsDecoded, "all 800 converter rows decode");
  }

  return runner.allPassed() ? 0 : 1;
}
