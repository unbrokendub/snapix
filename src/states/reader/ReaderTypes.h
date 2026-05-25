#pragma once

#include <cstdint>
#include <memory>
#include <string>

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

enum class ReaderNavigationJobKind : uint8_t {
  None,
  TocJump,
  PageLoad,
};

enum class ReaderNavigationJobVisibility : uint8_t {
  BlockingOverlay,
  DeferredDisplay,
};

enum class ReaderNavigationBlockReason : uint8_t {
  None,
  PageLoadNoProgress,
};

struct ReaderNavigationJob {
  ReaderNavigationJobKind kind = ReaderNavigationJobKind::None;
  ReaderNavigationJobVisibility visibility = ReaderNavigationJobVisibility::BlockingOverlay;

  bool tocJumpActive = false;
  bool tocJumpIndexingShown = false;
  bool tocJumpDeferredDisplay = false;
  int tocJumpTargetSpine = -1;
  int tocJumpTargetPageHint = -1;
  std::string tocJumpAnchor;
  uint8_t tocJumpRetryCount = 0;
  uint32_t tocJumpStartedMs = 0;
  uint32_t tocJumpLastDiagMs = 0;
  bool tocFirstPageReady = false;

  bool pageLoadActive = false;
  bool pageLoadMessageShown = false;
  bool pageLoadRequireComplete = false;
  bool pageLoadUseIndexingMessage = false;
  int pageLoadTargetSpine = -1;
  int pageLoadTargetPage = 0;
  uint8_t pageLoadRetryCount = 0;
  uint32_t pageLoadStartedMs = 0;
  uint32_t pageLoadLastDiagMs = 0;
  uint32_t pageLoadNextRetryMs = 0;

  int queuedTurn = 0;
  uint32_t queuedTurnQueuedMs = 0;
  bool queuedTurnHasQueuedMs = false;
  uint32_t lastCachePreemptRequestedMs = 0;
  bool deferredTurnAwaitingWorkerIdle = false;
  bool deferredTurnIdleLogged = false;

  ReaderNavigationBlockReason blockedReason = ReaderNavigationBlockReason::None;
  int blockedSpine = -1;
  int blockedPage = -1;
  uint8_t blockedFailures = 0;
  uint32_t blockedAtMs = 0;

  bool active() const { return kind != ReaderNavigationJobKind::None; }
  bool blocksInput() const {
    return kind == ReaderNavigationJobKind::TocJump && !tocJumpDeferredDisplay;
  }
  bool tocDeferredDisplay() const {
    return kind == ReaderNavigationJobKind::TocJump && tocJumpDeferredDisplay;
  }
  bool blocksPageLoad(int spine, int page) const {
    return blockedReason != ReaderNavigationBlockReason::None && blockedSpine == spine && blockedPage == page;
  }

  void clearTocJump() {
    if (kind == ReaderNavigationJobKind::TocJump) {
      kind = ReaderNavigationJobKind::None;
      visibility = ReaderNavigationJobVisibility::BlockingOverlay;
    }
    tocJumpActive = false;
    tocJumpIndexingShown = false;
    tocJumpDeferredDisplay = false;
    tocJumpTargetSpine = -1;
    tocJumpTargetPageHint = -1;
    tocJumpAnchor.clear();
    tocJumpRetryCount = 0;
    tocJumpStartedMs = 0;
    tocJumpLastDiagMs = 0;
    tocFirstPageReady = false;
  }

  void clearPageLoad() {
    if (kind == ReaderNavigationJobKind::PageLoad) {
      kind = ReaderNavigationJobKind::None;
      visibility = ReaderNavigationJobVisibility::BlockingOverlay;
    }
    pageLoadActive = false;
    pageLoadMessageShown = false;
    pageLoadRequireComplete = false;
    pageLoadUseIndexingMessage = false;
    pageLoadTargetSpine = -1;
    pageLoadTargetPage = 0;
    pageLoadRetryCount = 0;
    pageLoadStartedMs = 0;
    pageLoadLastDiagMs = 0;
    pageLoadNextRetryMs = 0;
  }

  void clearQueuedTurn() {
    queuedTurn = 0;
    queuedTurnQueuedMs = 0;
    queuedTurnHasQueuedMs = false;
    lastCachePreemptRequestedMs = 0;
    deferredTurnAwaitingWorkerIdle = false;
    deferredTurnIdleLogged = false;
  }

  void clearBlocked() {
    blockedReason = ReaderNavigationBlockReason::None;
    blockedSpine = -1;
    blockedPage = -1;
    blockedFailures = 0;
    blockedAtMs = 0;
  }

  void clear() {
    clearTocJump();
    clearPageLoad();
    clearQueuedTurn();
    clearBlocked();
    kind = ReaderNavigationJobKind::None;
    visibility = ReaderNavigationJobVisibility::BlockingOverlay;
  }
};

}  // namespace snapix::reader
