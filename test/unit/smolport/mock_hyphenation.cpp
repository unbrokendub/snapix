// Host-test mock for the Hyphenation lib.
//
// StreamingPaginator.cpp (v3.3.0) calls Hyphenation::setLanguage /
// breakOffsets for word hyphenation.  Linking the real lib into the host test
// would pull in the multi-hundred-KB language tries; instead this mock
// provides DETERMINISTIC break points so the paginator's split logic
// (prefix-fits selection, remainder placement, intra-page gate) can be unit
// tested with synthetic words.
//
// breakOffsets offers a break BETWEEN every pair of characters from offset 3
// to len-3 (ASCII test words only — one byte per char), each requesting a
// visible inserted hyphen.  The paginator picks the rightmost that fits.

#include "Hyphenation.h"

namespace Hyphenation {

void setLanguage(const std::string& /*lang*/) {}

std::vector<BreakInfo> breakOffsets(const std::string& word, bool /*includeFallback*/) {
  std::vector<BreakInfo> breaks;
  if (word.size() < 6) return breaks;
  for (size_t i = 3; i + 3 <= word.size(); ++i) {
    breaks.push_back({i, /*requiresInsertedHyphen=*/true});
  }
  return breaks;
}

}  // namespace Hyphenation
