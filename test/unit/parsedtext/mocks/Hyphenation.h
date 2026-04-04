#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Stub Hyphenation for ParsedText unit tests — no actual hyphenation.
namespace Hyphenation {

struct BreakInfo {
  size_t byteOffset;
  bool requiresInsertedHyphen;
};

inline std::vector<BreakInfo> breakOffsets(const std::string&, bool) { return {}; }
inline void setLanguage(const std::string&) {}

}  // namespace Hyphenation
