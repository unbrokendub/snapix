#pragma once

#include <cstdint>
#include <string>

#include "Utf8Nfc.h"

uint32_t utf8NextCodepoint(const unsigned char** string);

inline bool utf8IsCombiningMark(const uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F) ||  // Combining Diacritical Marks
         (cp >= 0x1DC0 && cp <= 0x1DFF) ||  // Supplement
         (cp >= 0x20D0 && cp <= 0x20FF) ||  // For Symbols
         (cp >= 0xFE20 && cp <= 0xFE2F);    // Half Marks
}

/**
 * UTF-8 safe string truncation - removes one character from the end.
 * Returns the new size after removing one UTF-8 character.
 */
size_t utf8RemoveLastChar(std::string& str);

/**
 * UTF-8 safe truncation - removes N characters from the end.
 */
void utf8TruncateChars(std::string& str, size_t numChars);

/**
 * v2.0.75: UTF-8 safe strncpy variant for fixed-size metadata buffers.
 *
 * Copies `src` into `dst` (null-terminated) without splitting a multi-byte
 * UTF-8 character at the end.  Pre-2.0.75 every metadata write went through
 * raw `strncpy(dst, src, sizeof(dst)-1); dst[sizeof(dst)-1]='\0'`, which
 * truncated Cyrillic/CJK/emoji titles mid-sequence and produced mojibake
 * (e.g. an Author field "Лев Толстой" cut at byte 1 of the cyrillic 'й'
 * displayed as "Лев Толсто" + a stray byte the renderer skipped or showed
 * as a replacement glyph).
 *
 * `dstSize` is the FULL buffer size including the null terminator.
 * Always writes a null at dst[written], where written ≤ dstSize-1 and
 * lies on a UTF-8 boundary.  Returns the byte count written (excluding null).
 */
size_t utf8SafeCopy(char* dst, size_t dstSize, const char* src);
