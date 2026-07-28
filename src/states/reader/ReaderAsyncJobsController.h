#pragma once

#include <BackgroundTask.h>

#include <freertos/queue.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

#include "ReaderSupport.h"
#include "ReaderTypes.h"

namespace snapix::reader {

class ReaderAsyncJobsController {
 public:
  using AbortCallback = std::function<bool()>;

  struct BackgroundCacheRequest {
    PositionState position;
    BackgroundCachePlan plan;
    bool showStatusBar = false;
    char trigger[32] = {};
  };

  struct TocJumpRequest {
    int targetSpine = -1;
    int targetPageHint = -1;
    uint8_t retryCount = 0;
    char anchor[160] = {};
  };

  struct PageFillRequest {
    int targetSpine = -1;
    int targetPage = 0;
    bool requireComplete = false;
  };

  using BackgroundCacheHandler = std::function<void(const BackgroundCacheRequest&, const AbortCallback&)>;
  using TocJumpHandler = std::function<void(const TocJumpRequest&, const AbortCallback&)>;
  using PageFillHandler = std::function<void(const PageFillRequest&, const AbortCallback&)>;

  ReaderAsyncJobsController();
  ~ReaderAsyncJobsController();

  bool startWorker();
  bool stopWorker();
  bool waitUntilIdle(uint32_t maxWaitMs = 0);

  bool isJobRunning() const;
  BackgroundTask::State workerState() const { return workerTask_.getState(); }
  void requestCancelCurrentJob();

  void setBackgroundCacheHandler(BackgroundCacheHandler handler) { backgroundCacheHandler_ = std::move(handler); }
  void setTocJumpHandler(TocJumpHandler handler) { tocJumpHandler_ = std::move(handler); }
  void setPageFillHandler(PageFillHandler handler) { pageFillHandler_ = std::move(handler); }

  bool queueBackgroundCache(const BackgroundCacheRequest& request);
  bool queueTocJumpWork(const TocJumpRequest& request);
  bool queuePageFillWork(const PageFillRequest& request);

  int& queuedPendingPageTurnRef() { return navigationJob_.queuedTurn; }
  uint32_t& queuedPendingPageTurnQueuedMsRef() { return navigationJob_.queuedTurnQueuedMs; }
  uint32_t& lastCachePreemptRequestedMsRef() { return navigationJob_.lastCachePreemptRequestedMs; }
  void markCachePreemptRequested(uint32_t nowMs) { navigationJob_.lastCachePreemptRequestedMs = nowMs; }
  void clearCachePreemptRequested() { navigationJob_.lastCachePreemptRequestedMs = 0; }
  void enqueuePendingPageTurn(int direction, const char* reason, int workerState);
  bool deferPageTurnUntilWorkerStops(int direction, bool workerRunning, int workerState,
                                     const std::function<void()>& requestStop);
  void noteQueuedTurnWorkerIdle(bool workerRunning);
  bool tryConsumeQueuedTurns(bool workerRunning, bool needsRender, bool pendingTocJump, bool pendingPageLoad,
                             bool menuMode, bool bookmarkMode, bool tocMode, int& queuedTurns,
                             uint32_t& queuedForMs);
  void clearQueuedPageTurns() { navigationJob_.clearQueuedTurn(); }
  bool pageLoadBlocked(int spine, int page) const { return navigationJob_.blocksPageLoad(spine, page); }
  void markPageLoadBlocked(int spine, int page, ReaderNavigationBlockReason reason);
  void clearPageLoadBlock() { navigationJob_.clearBlocked(); }

  bool navigationJobActive() const { return navigationJob_.active(); }
  bool navigationJobBlocksInput() const { return navigationJob_.blocksInput(); }
  bool navigationJobTocDeferredDisplay() const { return navigationJob_.tocDeferredDisplay(); }

  bool pendingTocJumpActive() const { return navigationJob_.tocJumpActive; }
  bool& pendingTocJumpActiveRef() { return navigationJob_.tocJumpActive; }
  bool pendingTocJumpIndexingShown() const { return navigationJob_.tocJumpIndexingShown; }
  bool& pendingTocJumpIndexingShownRef() { return navigationJob_.tocJumpIndexingShown; }
  bool pendingTocJumpDeferredDisplay() const { return navigationJob_.tocJumpDeferredDisplay; }
  bool& pendingTocJumpDeferredDisplayRef() { return navigationJob_.tocJumpDeferredDisplay; }
  void setPendingTocJumpDeferredDisplay(bool value) {
    navigationJob_.tocJumpDeferredDisplay = value;
    navigationJob_.visibility = value ? ReaderNavigationJobVisibility::DeferredDisplay
                                      : ReaderNavigationJobVisibility::BlockingOverlay;
  }
  int pendingTocJumpTargetSpine() const { return navigationJob_.tocJumpTargetSpine; }
  int& pendingTocJumpTargetSpineRef() { return navigationJob_.tocJumpTargetSpine; }
  int pendingTocJumpTargetPageHint() const { return navigationJob_.tocJumpTargetPageHint; }
  int& pendingTocJumpTargetPageHintRef() { return navigationJob_.tocJumpTargetPageHint; }
  const std::string& pendingTocJumpAnchor() const { return navigationJob_.tocJumpAnchor; }
  std::string& pendingTocJumpAnchorRef() { return navigationJob_.tocJumpAnchor; }
  uint8_t pendingTocJumpRetryCount() const { return navigationJob_.tocJumpRetryCount; }
  uint8_t& pendingTocJumpRetryCountRef() { return navigationJob_.tocJumpRetryCount; }
  uint32_t pendingTocJumpStartedMs() const { return navigationJob_.tocJumpStartedMs; }
  uint32_t& pendingTocJumpStartedMsRef() { return navigationJob_.tocJumpStartedMs; }
  uint32_t pendingTocJumpLastDiagMs() const { return navigationJob_.tocJumpLastDiagMs; }
  uint32_t& pendingTocJumpLastDiagMsRef() { return navigationJob_.tocJumpLastDiagMs; }
  void setPendingTocJumpLastDiagMs(uint32_t value) { navigationJob_.tocJumpLastDiagMs = value; }
  // Worker-to-main handoff. Byte-sized hardware accesses do not make a C++
  // data race well-defined, even on the single-core ESP32-C3.
  std::atomic<bool>& pendingTocFirstPageReadyRef() { return navigationJob_.tocFirstPageReady; }
  bool pendingTocFirstPageReady() const {
    return navigationJob_.tocFirstPageReady.load(std::memory_order_acquire);
  }
  void setPendingTocFirstPageReady(bool value) {
    navigationJob_.tocFirstPageReady.store(value, std::memory_order_release);
  }
  void clearPendingTocJump();
  void armPendingTocJump(int targetSpine, const std::string& anchor, int targetPageHint = -1);
  void incrementPendingTocJumpRetry() { navigationJob_.tocJumpRetryCount++; }
  void decrementPendingTocJumpRetry();

  bool pendingPageLoadActive() const { return navigationJob_.pageLoadActive; }
  bool& pendingPageLoadActiveRef() { return navigationJob_.pageLoadActive; }
  bool pendingPageLoadMessageShown() const { return navigationJob_.pageLoadMessageShown; }
  bool& pendingPageLoadMessageShownRef() { return navigationJob_.pageLoadMessageShown; }
  bool pendingPageLoadRequireComplete() const { return navigationJob_.pageLoadRequireComplete; }
  bool& pendingPageLoadRequireCompleteRef() { return navigationJob_.pageLoadRequireComplete; }
  bool pendingPageLoadUseIndexingMessage() const { return navigationJob_.pageLoadUseIndexingMessage; }
  bool& pendingPageLoadUseIndexingMessageRef() { return navigationJob_.pageLoadUseIndexingMessage; }
  int pendingPageLoadTargetSpine() const { return navigationJob_.pageLoadTargetSpine; }
  int& pendingPageLoadTargetSpineRef() { return navigationJob_.pageLoadTargetSpine; }
  int pendingPageLoadTargetPage() const { return navigationJob_.pageLoadTargetPage; }
  int& pendingPageLoadTargetPageRef() { return navigationJob_.pageLoadTargetPage; }
  uint8_t pendingPageLoadRetryCount() const { return navigationJob_.pageLoadRetryCount; }
  uint8_t& pendingPageLoadRetryCountRef() { return navigationJob_.pageLoadRetryCount; }
  uint32_t pendingPageLoadStartedMs() const { return navigationJob_.pageLoadStartedMs; }
  uint32_t& pendingPageLoadStartedMsRef() { return navigationJob_.pageLoadStartedMs; }
  uint32_t pendingPageLoadLastDiagMs() const { return navigationJob_.pageLoadLastDiagMs; }
  uint32_t& pendingPageLoadLastDiagMsRef() { return navigationJob_.pageLoadLastDiagMs; }
  void setPendingPageLoadLastDiagMs(uint32_t value) { navigationJob_.pageLoadLastDiagMs = value; }
  uint32_t pendingPageLoadNextRetryMs() const { return navigationJob_.pageLoadNextRetryMs; }
  uint32_t& pendingPageLoadNextRetryMsRef() { return navigationJob_.pageLoadNextRetryMs; }
  void clearPendingPageLoad();
  void armPendingPageLoad(int targetSpine, int targetPage, bool requireComplete, bool useIndexingMessage);
  void incrementPendingPageLoadRetry() { navigationJob_.pageLoadRetryCount++; }
  void decrementPendingPageLoadRetry();
  void setPendingPageLoadMessageShown(bool value) { navigationJob_.pageLoadMessageShown = value; }

  PendingRefreshState& pendingRefresh() { return pendingRefresh_; }
  const PendingRefreshState& pendingRefresh() const { return pendingRefresh_; }

  // v2.0.69: UI-cancel-only abort callback for the JPEG decode path.
  // Decode is mostly streaming after the arena allocation — extra
  // allocations during MCU iteration are tiny (LittleFS write buffer
  // ~256-512 B, no heap allocations in the JPEGDEC inner loop).
  // Aborting on free<15K mid-decode wastes a successful decode that
  // would have completed within seconds; the user then sees a stuck
  // placeholder.  This variant skips heap thresholds so decode runs
  // to completion unless the user explicitly preempts via button.
  // Public so ReaderState::runBackgroundCacheJob can swap in the
  // looser callback for fb2->decodePendingImages() while keeping the
  // strict abortCallback() (private, used by workerLoop) for parser
  // and cache-extend work.
  AbortCallback abortCallbackUiOnly() const;

 private:
  enum class JobType : uint8_t {
    None,
    BackgroundCache,
    TocJump,
    PageFill,
  };

  struct Command {
    JobType type = JobType::None;
    uint32_t generation = 0;
    BackgroundCacheRequest background;
    TocJumpRequest tocJump;
    PageFillRequest pageFill;
  };

  void workerLoop();
  bool enqueue(const Command& cmd);
  AbortCallback abortCallback(uint32_t generation) const;
  size_t clearQueuedCommands();
  uint16_t decrementOutstanding(uint16_t count);

  static constexpr EventBits_t EVENT_IDLE = (1 << 0);

  BackgroundTask workerTask_;
  QueueHandle_t commandQueue_ = nullptr;
  EventGroupHandle_t stateEvents_ = nullptr;
  std::atomic<JobType> currentJob_{JobType::None};
  std::atomic<uint16_t> outstandingJobs_{0};
  std::atomic<uint32_t> cancelGeneration_{0};
  std::atomic<uint32_t> currentJobGeneration_{0};

  BackgroundCacheHandler backgroundCacheHandler_;
  TocJumpHandler tocJumpHandler_;
  PageFillHandler pageFillHandler_;

  ReaderNavigationJob navigationJob_;

  PendingRefreshState pendingRefresh_;
};

}  // namespace snapix::reader
