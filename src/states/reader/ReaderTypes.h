#pragma once

#include <cstdint>
#include <memory>

class Page;

namespace snapix::reader {

struct PositionState {
  uint32_t currentPage = 0;
  int currentSpineIndex = 0;
  int currentSectionPage = 0;
  int lastRenderedSpineIndex = 0;
  int lastRenderedSectionPage = 0;
  bool hasCover = false;
  int textStartIndex = 0;
};

struct PositionRefs {
  uint32_t& currentPage;
  int& currentSpineIndex;
  int& currentSectionPage;
  int& lastRenderedSpineIndex;
  int& lastRenderedSectionPage;
  bool& hasCover;
  int& textStartIndex;
};

struct Viewport {
  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
  int width = 0;
  int height = 0;
};

struct WarmPageSlot {
  int spineIndex = -1;
  int sectionPage = -1;
  uint16_t pageCount = 0;
  bool isPartial = false;
  std::shared_ptr<Page> page;

  bool matches(int spine, int pageNum) const { return page && spineIndex == spine && sectionPage == pageNum; }

  void clear() {
    spineIndex = -1;
    sectionPage = -1;
    pageCount = 0;
    isPartial = false;
    page.reset();
  }
};

enum class BackgroundCacheWakeReason : uint8_t {
  None,
  Thumbnail,
  CurrentCacheMissing,
  CurrentCachePartial,
  CurrentCachePartialWaiting,
  NearPrefetchReady,
  FarPrefetchReady,
  BlockedByHeap,
};

struct BackgroundCachePlan {
  bool shouldStart = false;
  BackgroundCacheWakeReason reason = BackgroundCacheWakeReason::None;
  int activeSpine = -1;
  int candidateSpine = -1;
  bool allowFarSweep = false;
  bool currentPartialCanHotExtend = false;
  bool currentPartialNearTail = false;
};

struct PendingRefreshState {
  bool active = false;
  int spine = -1;
  int page = -1;

  void clear() {
    active = false;
    spine = -1;
    page = -1;
  }
};

}  // namespace snapix::reader
