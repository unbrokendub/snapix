/**
 * TxtEncoding.h
 *
 * Encoding detection + cp1251 → UTF-8 conversion for TXT files.
 *
 * v2.0.187 — adds support for Windows-1251 (Russian Cyrillic) encoded
 * .txt files commonly produced by royallib.ru, lib.ru, and other
 * Russian book sites.  Pre-fix such files rendered as `?` placeholders
 * because the downstream renderer assumes UTF-8 input.
 *
 * Strategy:
 *   1. Sample first 4 KB of the file.
 *   2. Walk bytes checking UTF-8 well-formedness.
 *   3. If file contains high-bit bytes (0x80+) AND any of them form
 *      invalid UTF-8 sequences → classify as cp1251.
 *   4. Caller can then convert the file to a cached UTF-8 copy via
 *      `convertCp1251FileToUtf8()` and read from that instead.
 *
 * Limitations:
 *   * Only cp1251 vs UTF-8 distinction.  KOI8-R, cp866, etc. would
 *     also classify as cp1251 (probably rendering wrong but at least
 *     producing valid UTF-8 garbage rather than `?`).  Most Russian
 *     book sites use cp1251, so this covers the common case.
 *   * Detection is heuristic — pathological inputs (e.g., binary
 *     data with bytes that happen to look UTF-8-valid) could be
 *     misclassified.  Acceptable for the intended use (plain text).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace snapix::txt {

enum class Encoding : uint8_t {
  Utf8,       // ASCII or valid UTF-8 (the renderer's native expectation)
  Cp1251,     // Windows-1251 Cyrillic; needs conversion before render
};

/**
 * Detect encoding of a file by sampling its first `sampleBytes` bytes.
 * `filepath` is opened on the SD card via SdMan.
 *
 * Returns `Encoding::Utf8` for pure ASCII files (no high-bit bytes at
 * all) and well-formed UTF-8 files.  Returns `Encoding::Cp1251` if the
 * sample contains high-bit bytes that aren't part of valid UTF-8
 * sequences.
 *
 * Pure-ASCII files default to Utf8 — they're a strict subset of UTF-8.
 */
Encoding detectFileEncoding(const std::string& filepath, std::size_t sampleBytes = 4096);

/**
 * Convert an SD-resident cp1251-encoded file to a UTF-8 cache file on
 * LittleFS.  Reads `srcPath` from SD via SdMan, writes `dstPath` to
 * LittleFS.  Streams in chunks (8 KB) so heap usage stays bounded
 * regardless of source file size.
 *
 * Returns `true` on success.  On any I/O failure, returns `false` and
 * removes the (possibly partial) `dstPath` to avoid stale-cache state.
 *
 * The output file is typically 1.5-2x larger than the input because
 * Cyrillic codepoints (U+0400-U+04FF) encode as 2 bytes in UTF-8 vs
 * 1 byte in cp1251.
 */
bool convertCp1251FileToUtf8(const std::string& srcPath, const std::string& dstPath);

}  // namespace snapix::txt
