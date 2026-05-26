/**
 * TxtEncoding.cpp — v2.0.187 Windows-1251 → UTF-8 support for TXT files.
 *
 * Architecture lives in TxtEncoding.h.  This translation unit holds the
 * detection state machine, the cp1251→Unicode mapping table, and the
 * streaming converter.
 */

#include "TxtEncoding.h"

#include <FS.h>
#include <LittleFS.h>
#include <Logging.h>
#include <SDCardManager.h>

#include <cstring>

#define TAG "TXT_ENC"

namespace snapix::txt {

namespace {

// ============================================================================
// Windows-1251 → Unicode codepoint mapping for the high half (0x80-0xFF).
//
// Compact 256-byte table (128 entries × uint16_t).  Most entries fall
// in U+0400-U+04FF (Cyrillic) which encodes as 2 UTF-8 bytes; a few
// (€, …, †, etc.) live in U+2000-U+20FF (general punctuation) which
// encode as 3 UTF-8 bytes.  Codepoint 0x0000 marks the one undefined
// slot in cp1251 (0x98) — we emit U+FFFD (replacement char) for it.
//
// Source: Microsoft's cp1251 reference table, cross-checked against
// Unicode's WINDOWS/CP1251.TXT mapping file.
// ============================================================================
constexpr uint16_t kCp1251HighToUnicode[128] = {
    /* 0x80 */ 0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    /* 0x88 */ 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    /* 0x90 */ 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    /* 0x98 */ 0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    /* 0xA0 */ 0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    /* 0xA8 */ 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    /* 0xB0 */ 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    /* 0xB8 */ 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    /* 0xC0 */ 0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    /* 0xC8 */ 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    /* 0xD0 */ 0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    /* 0xD8 */ 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    /* 0xE0 */ 0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    /* 0xE8 */ 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    /* 0xF0 */ 0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    /* 0xF8 */ 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
};

// Encode a Unicode codepoint as UTF-8.  Writes 1-3 bytes to `out`,
// returns the byte count written.  Codepoints outside BMP (>= 0x10000)
// don't occur in cp1251, so we don't bother with the 4-byte form.
inline int encodeUtf8(uint16_t cp, uint8_t* out) {
  if (cp < 0x80) {
    out[0] = static_cast<uint8_t>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
    out[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    return 2;
  }
  // 0x800 - 0xFFFF (3-byte UTF-8 — includes Cyrillic and most cp1251
  // punctuation symbols like €, …, †, ™).
  out[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
  out[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
  out[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  return 3;
}

// UTF-8 lead-byte classification for the detection state machine.
// Returns the number of continuation bytes expected after `b`, or 0
// for ASCII, or -1 for an invalid lead byte (could be a continuation
// byte appearing alone, or an out-of-range lead).
inline int utf8ExpectedContinuations(uint8_t b) {
  if (b < 0x80) return 0;
  if ((b & 0xE0) == 0xC0) return 1;   // 110xxxxx
  if ((b & 0xF0) == 0xE0) return 2;   // 1110xxxx
  if ((b & 0xF8) == 0xF0) return 3;   // 11110xxx
  return -1;                          // 10xxxxxx alone, or 11111xxx
}

}  // namespace

Encoding detectFileEncoding(const std::string& filepath, std::size_t sampleBytes) {
  FsFile file;
  if (!SdMan.openFileForRead("TXT_ENC", filepath, file)) {
    // Can't even open the file — caller will get an error anyway when
    // it tries to load().  Default to UTF-8 (no conversion needed).
    return Encoding::Utf8;
  }

  uint8_t buf[512];
  std::size_t totalRead = 0;
  bool anyHighByte = false;
  std::size_t invalidUtf8 = 0;
  std::size_t carryBytes = 0;  // partial UTF-8 sequence from previous chunk
  int carryExpected = 0;       // continuation bytes still expected

  while (totalRead < sampleBytes) {
    const int n = file.read(buf, sizeof(buf));
    if (n <= 0) break;
    const std::size_t chunk = static_cast<std::size_t>(n);
    totalRead += chunk;

    std::size_t i = 0;
    // Drain any UTF-8 continuation bytes carried over from the prior
    // chunk's tail-truncated lead sequence.
    while (carryExpected > 0 && i < chunk) {
      if ((buf[i] & 0xC0) != 0x80) {
        invalidUtf8++;
        carryExpected = 0;
        break;  // re-process this byte as a fresh lead in the main loop
      }
      i++;
      carryExpected--;
    }

    while (i < chunk) {
      const uint8_t b = buf[i];
      if (b < 0x80) {
        i++;
        continue;
      }
      anyHighByte = true;
      const int expected = utf8ExpectedContinuations(b);
      if (expected < 0) {
        invalidUtf8++;
        i++;
        continue;
      }
      // Verify continuation bytes for this lead, accounting for chunk
      // boundary (may need to carry into the next chunk).
      bool ok = true;
      for (int c = 1; c <= expected; c++) {
        if (i + c >= chunk) {
          // Partial sequence at end of chunk — assume valid; the next
          // chunk's drain loop will verify.
          carryExpected = expected - c + 1;
          break;
        }
        if ((buf[i + c] & 0xC0) != 0x80) {
          ok = false;
          break;
        }
      }
      if (!ok) {
        invalidUtf8++;
        i++;
      } else if (carryExpected > 0) {
        // We deferred verification of (expected - carryExpected + 1)
        // continuations to the next chunk.  Advance past what we have.
        i = chunk;
      } else {
        i += expected + 1;
      }
    }
    carryBytes = carryExpected > 0 ? 1 : 0;
    (void)carryBytes;  // silence unused-var warning in some builds
  }
  file.close();

  if (!anyHighByte) {
    LOG_DBG(TAG, "Detected ASCII (treating as UTF-8): %s", filepath.c_str());
    return Encoding::Utf8;
  }
  if (invalidUtf8 > 0) {
    LOG_INF(TAG, "Detected cp1251 (UTF-8 invalid count=%zu in %zu-byte sample): %s",
            invalidUtf8, totalRead, filepath.c_str());
    return Encoding::Cp1251;
  }
  LOG_DBG(TAG, "Detected UTF-8 (sample=%zu bytes, all high bytes well-formed): %s",
          totalRead, filepath.c_str());
  return Encoding::Utf8;
}

bool convertCp1251FileToUtf8(const std::string& srcPath, const std::string& dstPath) {
  FsFile src;
  if (!SdMan.openFileForRead("TXT_ENC", srcPath, src)) {
    LOG_ERR(TAG, "convert: failed to open source: %s", srcPath.c_str());
    return false;
  }

  // LittleFS "w" creates / truncates.
  File dst = LittleFS.open(dstPath.c_str(), "w");
  if (!dst) {
    LOG_ERR(TAG, "convert: failed to open dest: %s", dstPath.c_str());
    src.close();
    return false;
  }

  // v2.0.188 — HEAP-allocate the I/O buffers instead of putting them on
  // the stack.  v2.0.187 used `uint8_t inBuf[8192]; uint8_t outBuf[24576]`
  // — 32 KB of stack arrays on top of the caller's frame.  loopTask's
  // stack is 24 KB (v2.0.184), so a single conversion guaranteed an
  // overflow: hardware repro showed SP 9.8 KB below the lower bound
  // during the very first cp1251 conversion on the user's Asimov TXT.
  //
  // Heap alloc per-call is fine here — conversion runs once per cp1251
  // file load (not per chunk, not per page), and the 32 KB free at
  // function exit.  malloc/free are ~µs each; the conversion itself
  // takes 100-300 ms for typical book sizes, so the alloc overhead
  // is in the noise.  The two buffers stay allocated together for the
  // duration of the conversion, then both freed in lockstep.
  constexpr std::size_t kReadChunk = 8192;
  constexpr std::size_t kWriteChunk = kReadChunk * 3;
  uint8_t* inBuf = static_cast<uint8_t*>(malloc(kReadChunk));
  uint8_t* outBuf = static_cast<uint8_t*>(malloc(kWriteChunk));
  if (!inBuf || !outBuf) {
    LOG_ERR(TAG, "convert: failed to allocate %zu+%zu bytes heap for conversion buffers",
            kReadChunk, kWriteChunk);
    if (inBuf) free(inBuf);
    if (outBuf) free(outBuf);
    src.close();
    dst.close();
    LittleFS.remove(dstPath.c_str());
    return false;
  }

  std::size_t srcTotal = 0;
  std::size_t dstTotal = 0;
  bool ioOk = true;

  while (ioOk) {
    const int n = src.read(inBuf, kReadChunk);
    if (n <= 0) break;
    const std::size_t inLen = static_cast<std::size_t>(n);
    srcTotal += inLen;

    std::size_t outLen = 0;
    for (std::size_t i = 0; i < inLen; i++) {
      const uint8_t b = inBuf[i];
      if (b < 0x80) {
        // Pure ASCII — keep as-is.
        outBuf[outLen++] = b;
      } else {
        uint16_t cp = kCp1251HighToUnicode[b - 0x80];
        if (cp == 0x0000) {
          // Undefined cp1251 slot (0x98).  Emit U+FFFD replacement.
          cp = 0xFFFD;
        }
        outLen += encodeUtf8(cp, outBuf + outLen);
      }
    }
    const std::size_t written = dst.write(outBuf, outLen);
    if (written != outLen) {
      LOG_ERR(TAG, "convert: short write at src=%zu (want=%zu got=%zu): %s",
              srcTotal, outLen, written, dstPath.c_str());
      ioOk = false;
      break;
    }
    dstTotal += written;
  }

  dst.flush();
  dst.close();
  src.close();

  // v2.0.188 — release heap buffers regardless of outcome.
  free(inBuf);
  free(outBuf);

  if (!ioOk) {
    LittleFS.remove(dstPath.c_str());
    return false;
  }

  LOG_INF(TAG, "Converted cp1251 → UTF-8: %s -> %s (%zu -> %zu bytes, %.1fx)",
          srcPath.c_str(), dstPath.c_str(), srcTotal, dstTotal,
          srcTotal > 0 ? static_cast<double>(dstTotal) / static_cast<double>(srcTotal) : 0.0);
  return true;
}

}  // namespace snapix::txt
