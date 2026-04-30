#include "ReaderAsyncJobsController.h"

#include <Logging.h>
#include <esp_heap_caps.h>

#include <new>

#define TAG "RDR_ASYNC"

namespace snapix::reader {

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
  if (stateEvents_) {
    xEventGroupClearBits(stateEvents_, EVENT_IDLE);
  }
  if (xQueueSend(commandQueue_, &cmd, 0) != pdTRUE) {
    LOG_ERR(TAG, "[ASYNC] command queue full type=%d", static_cast<int>(cmd.type));
    if (!isJobRunning() && stateEvents_) {
      xEventGroupSetBits(stateEvents_, EVENT_IDLE);
    }
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
  return [this]() {
    if (cancelCurrentJob_.load(std::memory_order_acquire) || workerTask_.shouldStop()) return true;
    // Abort parsing early when heap is dangerously low to prevent std::bad_alloc.
    // Normal operating heap on ESP32-C3 is ~40-55 KB; the parser pipeline needs
    // ~5-10 KB working memory for text layout.  Threshold must stay well below
    // the steady-state level so normal extend/create operations are not blocked.
    const size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (freeBytes < 15 * 1024) {
      LOG_ERR(TAG, "Aborting job: heap dangerously low (%u bytes free)", static_cast<unsigned>(freeBytes));
      return true;
    }
    // Also check heap fragmentation.  Repeated hot extends keep the parser
    // alive between calls; its allocations sit in the middle of the heap
    // and prevent coalescing.  With pool-based TextBlock the parser's largest
    // single allocation is well under 4 KB, so 8 KB contiguous is sufficient.
    const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largestBlock < 8 * 1024) {
      LOG_ERR(TAG, "Aborting job: heap fragmented (largest=%u free=%u)",
              static_cast<unsigned>(largestBlock), static_cast<unsigned>(freeBytes));
      return true;
    }
    return false;
  };
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
    try {
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
    } catch (const std::bad_alloc&) {
      LOG_ERR(TAG, "OOM in worker (job=%d, heap=%u)", static_cast<int>(cmd.type),
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    } catch (...) {
      LOG_ERR(TAG, "Unhandled exception in worker (job=%d)", static_cast<int>(cmd.type));
    }

    currentJob_.store(JobType::None, std::memory_order_release);
    if (stateEvents_) {
      xEventGroupSetBits(stateEvents_, EVENT_IDLE);
    }
  }
}

}  // namespace snapix::reader
