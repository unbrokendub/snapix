#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

namespace snapix {

// Deterministic, case-insensitive natural order for sleep image names:
// image2.bmp comes before image10.bmp.  The lexical tie-breaker keeps names
// that differ only by case or zero padding in a stable total order.
inline bool sleepImageFilenameLess(const std::string& lhs, const std::string& rhs) {
  const char* left = lhs.c_str();
  const char* right = rhs.c_str();
  const auto asUnsigned = [](const char value) { return static_cast<unsigned char>(value); };

  while (*left && *right) {
    if (std::isdigit(asUnsigned(*left)) && std::isdigit(asUnsigned(*right))) {
      while (*left == '0') ++left;
      while (*right == '0') ++right;

      std::size_t leftDigits = 0;
      std::size_t rightDigits = 0;
      while (std::isdigit(asUnsigned(left[leftDigits]))) ++leftDigits;
      while (std::isdigit(asUnsigned(right[rightDigits]))) ++rightDigits;

      if (leftDigits != rightDigits) return leftDigits < rightDigits;
      for (std::size_t i = 0; i < leftDigits; ++i) {
        if (left[i] != right[i]) return left[i] < right[i];
      }
      left += leftDigits;
      right += rightDigits;
      continue;
    }

    const int leftLower = std::tolower(asUnsigned(*left));
    const int rightLower = std::tolower(asUnsigned(*right));
    if (leftLower != rightLower) return leftLower < rightLower;
    ++left;
    ++right;
  }

  if (*left != *right) return *left == '\0';
  return lhs < rhs;
}

// Returns the image to show now and advances the persisted settings cursor.
// The modulo also makes the cursor safe when files are added or removed.
inline std::size_t takeNextSleepImageIndex(uint32_t& nextIndex, const std::size_t imageCount) {
  if (imageCount == 0) return 0;

  const std::size_t selected = static_cast<std::size_t>(nextIndex) % imageCount;
  nextIndex = static_cast<uint32_t>((selected + 1) % imageCount);
  return selected;
}

}  // namespace snapix
