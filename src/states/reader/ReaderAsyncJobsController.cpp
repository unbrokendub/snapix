#include "ReaderAsyncJobsController.h"

#include <Logging.h>

#define TAG "RDR_ASYNC"

namespace papyrix::reader {

ReaderAsyncJobsController::ReaderAsyncJobsController() {
  commandQueue_ = xQueueCreate(6, sizeof(Command));
  stateEvents_ = xEventGroupCreate();
  if (stateEvents_) {
    xEventGroupSetBits(stateEvents_, EVENT_IDLE);
  }
}

ReaderAsyncJobsController::~ReaderAsyncJobsController() {
  stopWorker();
  if (commandQueue_) {
    vQueueDelete(commandQueue_);
    commandQueue_ = nullptr;
  }
  if (stateEvents_) {
    vEventGroupDelete(stateEvents_);
    stateEvents_ = nullptr;
  }
}

bool ReaderAsyncJobsController::startWorker() {
  if (!commandQueue_ || !stateEvents_) {
    LOG_ERR(TAG, "[ASYNC] worker primitives unavailable");
    return false;
  }
  if (workerTask_.isRunning()) {
    return true;
  }
  clearQueuedCommands();
  return workerTask_.start("ReaderAsync", kCacheTaskStackSize, [this]() { workerLoop(); }, 1);
}

bool ReaderAsyncJobsController::stopWorker() {
  requestCancelCurrentJob();
  const bool stopped = workerTask_.stop(kCacheTaskStopTimeoutMs);
  if (stopped) {
    clearQueuedCommands();
    currentJob_.store(JobType::None, std::memory_order_release);
    cancelCurrentJob_.store(false, std::memory_order_release);
    if (stateEvents_) {
      xEventGroupSetBits(stateEvents_, EVENT_IDLE);
    }
  }
  return stopped;
}

bool ReaderAsyncJobsController::waitUntilIdle(const uint32_t maxWaitMs) {
  if (!stateEvents_) {
    return !isJobRunning();
  }
  const TickType_t waitTicks = maxWaitMs == 0 ? portMAX_DELAY : pdMS_TO_TICKS(maxWaitMs);
  const EventBits_t bits = xEventGroupWaitBits(stateEvents_, EVENT_IDLE, pdFALSE, pdTRUE, waitTicks);
  return (bits & EVENT_IDLE) != 0;
}

bool ReaderAsyncJobsController::isJobRunning() const { return currentJob_.load(std::memory_order_acquire) != JobType::None; }

void ReaderAsyncJobsController::requestCancelCurrentJob() { cancelCurrentJob_.store(true, std::memory_order_release); }

bool ReaderAsyncJobsController::queueBackgroundCache(const BackgroundCacheRequest& request) {
  Command cmd;
  cmd.type = JobType::BackgroundCache;
  cmd.background = request;
  return enqueue(cmd);
}

bool ReaderAsyncJobsController::queueTocJumpWork(const TocJumpRequest& request) {
  Command cmd;
  cmd.type = JobType::TocJump;
  cmd.tocJump = request;
  return enqueue(cmd);
}

bool ReaderAsyncJobsController::queuePageFillWork(const PageFillRequest& request) {
  Command cmd;
  cmd.type = JobType::PageFill;
  cmd.pageFill = request;
  return enqueue(cmd);
}

void ReaderAsyncJobsController::clearPendingTocJump() {
  pendingTocJumpActive_ = false;
  pendingTocJumpIndexingShown_ = false;
  pendingTocJumpTargetSpine_ = -1;
  pendingTocJumpTargetPageHint_ = -1;
  pendingTocJumpAnchor_.clear();
  pendingTocJumpRetryCount_ = 0;
  pendingTocJumpStartedMs_ = 0;
  pendingTocJumpLastDiagMs_ = 0;
}

void ReaderAsyncJobsController::armPendingTocJump(const int targetSpine, const std::string& anchor,
                                                  const int targetPageHint) {
  pendingTocJumpActive_ = true;
  pendingTocJumpIndexingShown_ = false;
  pendingTocJumpTargetSpine_ = targetSpine;
  pendingTocJumpTargetPageHint_ = targetPageHint;
  pendingTocJumpAnchor_ = anchor;
  pendingTocJumpRetryCount_ = 0;
  pendingTocJumpStartedMs_ = millis();
  pendingTocJumpLastDiagMs_ = 0;
}

void ReaderAsyncJobsController::decrementPendingTocJumpRetry() {
  if (pendingTocJumpRetryCount_ > 0) {
    pendingTocJumpRetryCount_--;
  }
}

void ReaderAsyncJobsController::clearPendingPageLoad() {
  pendingEpubPageLoadActive_ = false;
  pendingEpubPageLoadMessageShown_ = false;
  pendingEpubPageLoadRequireComplete_ = false;
  pendingEpubPageLoadUseIndexingMessage_ = false;
  pendingEpubPageLoadTargetSpine_ = -1;
  pendingEpubPageLoadTargetPage_ = 0;
  pendingEpubPageLoadRetryCount_ = 0;
  pendingEpubPageLoadStartedMs_ = 0;
  pendingEpubPageLoadLastDiagMs_ = 0;
}

void ReaderAsyncJobsController::armPendingPageLoad(const int targetSpine, const int targetPage, const bool requireComplete,
                                                   const bool useIndexingMessage) {
  pendingEpubPageLoadActive_ = true;
  pendingEpubPageLoadTargetSpine_ = targetSpine;
  pendingEpubPageLoadTargetPage_ = targetPage;
  pendingEpubPageLoadRequireComplete_ = requireComplete;
  pendingEpubPageLoadUseIndexingMessage_ = useIndexingMessage;
  pendingEpubPageLoadStartedMs_ = millis();
  pendingEpubPageLoadLastDiagMs_ = 0;
  pendingEpubPageLoadMessageShown_ = false;
}

void ReaderAsyncJobsController::decrementPendingPageLoadRetry() {
  if (pendingEpubPageLoadRetryCount_ > 0) {
    pendingEpubPageLoadRetryCount_--;
  }
}

bool ReaderAsyncJobsController::enqueue(const Command& cmd) {
  if (!commandQueue_) {
    return false;
  }
  if (!workerTask_.isRunning() && !startWorker()) {
    return false;
  }
  if (xQueueSend(commandQueue_, &cmd, 0) != pdTRUE) {
    LOG_ERR(TAG, "[ASYNC] command queue full type=%d", static_cast<int>(cmd.type));
    return false;
  }
  return true;
}

size_t ReaderAsyncJobsController::clearQueuedCommands() {
  if (!commandQueue_) {
    return 0;
  }

  size_t cleared = 0;
  Command discarded;
  while (xQueueReceive(commandQueue_, &discarded, 0) == pdTRUE) {
    ++cleared;
  }

  if (cleared > 0) {
    LOG_DBG(TAG, "[ASYNC] dropped %u stale queued command(s)", static_cast<unsigned>(cleared));
  }
  return cleared;
}

ReaderAsyncJobsController::AbortCallback ReaderAsyncJobsController::abortCallback() const {
  return [this]() { return cancelCurrentJob_.load(std::memory_order_acquire) || workerTask_.shouldStop(); };
}

void ReaderAsyncJobsController::workerLoop() {
  while (!workerTask_.shouldStop()) {
    Command cmd;
    if (xQueueReceive(commandQueue_, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
      continue;
    }

    cancelCurrentJob_.store(false, std::memory_order_release);
    currentJob_.store(cmd.type, std::memory_order_release);
    if (stateEvents_) {
      xEventGroupClearBits(stateEvents_, EVENT_IDLE);
    }

    const int priority = cmd.type == JobType::BackgroundCache ? 0 : kInteractiveCacheTaskPriority;
    vTaskPrioritySet(nullptr, priority);

    const AbortCallback abort = abortCallback();
    switch (cmd.type) {
      case JobType::BackgroundCache:
        if (backgroundCacheHandler_) backgroundCacheHandler_(cmd.background, abort);
        break;
      case JobType::TocJump:
        if (tocJumpHandler_) tocJumpHandler_(cmd.tocJump, abort);
        break;
      case JobType::PageFill:
        if (pageFillHandler_) pageFillHandler_(cmd.pageFill, abort);
        break;
      case JobType::None:
        break;
    }

    currentJob_.store(JobType::None, std::memory_order_release);
    if (stateEvents_) {
      xEventGroupSetBits(stateEvents_, EVENT_IDLE);
    }
  }
}

}  // namespace papyrix::reader
