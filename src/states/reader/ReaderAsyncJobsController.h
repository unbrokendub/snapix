#pragma once

#include <BackgroundTask.h>

#include <freertos/queue.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

#include "ReaderSupport.h"
#include "ReaderTypes.h"

namespace papyrix::reader {

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

  bool pendingTocJumpActive() const { return pendingTocJumpActive_; }
  bool& pendingTocJumpActiveRef() { return pendingTocJumpActive_; }
  bool pendingTocJumpIndexingShown() const { return pendingTocJumpIndexingShown_; }
  bool& pendingTocJumpIndexingShownRef() { return pendingTocJumpIndexingShown_; }
  int pendingTocJumpTargetSpine() const { return pendingTocJumpTargetSpine_; }
  int& pendingTocJumpTargetSpineRef() { return pendingTocJumpTargetSpine_; }
  int pendingTocJumpTargetPageHint() const { return pendingTocJumpTargetPageHint_; }
  int& pendingTocJumpTargetPageHintRef() { return pendingTocJumpTargetPageHint_; }
  const std::string& pendingTocJumpAnchor() const { return pendingTocJumpAnchor_; }
  std::string& pendingTocJumpAnchorRef() { return pendingTocJumpAnchor_; }
  uint8_t pendingTocJumpRetryCount() const { return pendingTocJumpRetryCount_; }
  uint8_t& pendingTocJumpRetryCountRef() { return pendingTocJumpRetryCount_; }
  uint32_t pendingTocJumpStartedMs() const { return pendingTocJumpStartedMs_; }
  uint32_t& pendingTocJumpStartedMsRef() { return pendingTocJumpStartedMs_; }
  uint32_t pendingTocJumpLastDiagMs() const { return pendingTocJumpLastDiagMs_; }
  uint32_t& pendingTocJumpLastDiagMsRef() { return pendingTocJumpLastDiagMs_; }
  void setPendingTocJumpLastDiagMs(uint32_t value) { pendingTocJumpLastDiagMs_ = value; }
  void clearPendingTocJump();
  void armPendingTocJump(int targetSpine, const std::string& anchor, int targetPageHint = -1);
  void incrementPendingTocJumpRetry() { pendingTocJumpRetryCount_++; }
  void decrementPendingTocJumpRetry();

  bool pendingPageLoadActive() const { return pendingEpubPageLoadActive_; }
  bool& pendingPageLoadActiveRef() { return pendingEpubPageLoadActive_; }
  bool pendingPageLoadMessageShown() const { return pendingEpubPageLoadMessageShown_; }
  bool& pendingPageLoadMessageShownRef() { return pendingEpubPageLoadMessageShown_; }
  bool pendingPageLoadRequireComplete() const { return pendingEpubPageLoadRequireComplete_; }
  bool& pendingPageLoadRequireCompleteRef() { return pendingEpubPageLoadRequireComplete_; }
  bool pendingPageLoadUseIndexingMessage() const { return pendingEpubPageLoadUseIndexingMessage_; }
  bool& pendingPageLoadUseIndexingMessageRef() { return pendingEpubPageLoadUseIndexingMessage_; }
  int pendingPageLoadTargetSpine() const { return pendingEpubPageLoadTargetSpine_; }
  int& pendingPageLoadTargetSpineRef() { return pendingEpubPageLoadTargetSpine_; }
  int pendingPageLoadTargetPage() const { return pendingEpubPageLoadTargetPage_; }
  int& pendingPageLoadTargetPageRef() { return pendingEpubPageLoadTargetPage_; }
  uint8_t pendingPageLoadRetryCount() const { return pendingEpubPageLoadRetryCount_; }
  uint8_t& pendingPageLoadRetryCountRef() { return pendingEpubPageLoadRetryCount_; }
  uint32_t pendingPageLoadStartedMs() const { return pendingEpubPageLoadStartedMs_; }
  uint32_t& pendingPageLoadStartedMsRef() { return pendingEpubPageLoadStartedMs_; }
  uint32_t pendingPageLoadLastDiagMs() const { return pendingEpubPageLoadLastDiagMs_; }
  uint32_t& pendingPageLoadLastDiagMsRef() { return pendingEpubPageLoadLastDiagMs_; }
  void setPendingPageLoadLastDiagMs(uint32_t value) { pendingEpubPageLoadLastDiagMs_ = value; }
  void clearPendingPageLoad();
  void armPendingPageLoad(int targetSpine, int targetPage, bool requireComplete, bool useIndexingMessage);
  void incrementPendingPageLoadRetry() { pendingEpubPageLoadRetryCount_++; }
  void decrementPendingPageLoadRetry();
  void setPendingPageLoadMessageShown(bool value) { pendingEpubPageLoadMessageShown_ = value; }

  PendingRefreshState& pendingRefresh() { return pendingRefresh_; }
  const PendingRefreshState& pendingRefresh() const { return pendingRefresh_; }

 private:
  enum class JobType : uint8_t {
    None,
    BackgroundCache,
    TocJump,
    PageFill,
  };

  struct Command {
    JobType type = JobType::None;
    BackgroundCacheRequest background;
    TocJumpRequest tocJump;
    PageFillRequest pageFill;
  };

  void workerLoop();
  bool enqueue(const Command& cmd);
  AbortCallback abortCallback() const;
  size_t clearQueuedCommands();

  static constexpr EventBits_t EVENT_IDLE = (1 << 0);

  BackgroundTask workerTask_;
  QueueHandle_t commandQueue_ = nullptr;
  EventGroupHandle_t stateEvents_ = nullptr;
  std::atomic<JobType> currentJob_{JobType::None};
  std::atomic<bool> cancelCurrentJob_{false};

  BackgroundCacheHandler backgroundCacheHandler_;
  TocJumpHandler tocJumpHandler_;
  PageFillHandler pageFillHandler_;

  bool pendingTocJumpActive_ = false;
  bool pendingTocJumpIndexingShown_ = false;
  int pendingTocJumpTargetSpine_ = -1;
  int pendingTocJumpTargetPageHint_ = -1;
  std::string pendingTocJumpAnchor_;
  uint8_t pendingTocJumpRetryCount_ = 0;
  uint32_t pendingTocJumpStartedMs_ = 0;
  uint32_t pendingTocJumpLastDiagMs_ = 0;

  bool pendingEpubPageLoadActive_ = false;
  bool pendingEpubPageLoadMessageShown_ = false;
  bool pendingEpubPageLoadRequireComplete_ = false;
  bool pendingEpubPageLoadUseIndexingMessage_ = false;
  int pendingEpubPageLoadTargetSpine_ = -1;
  int pendingEpubPageLoadTargetPage_ = 0;
  uint8_t pendingEpubPageLoadRetryCount_ = 0;
  uint32_t pendingEpubPageLoadStartedMs_ = 0;
  uint32_t pendingEpubPageLoadLastDiagMs_ = 0;

  PendingRefreshState pendingRefresh_;
};

}  // namespace papyrix::reader
