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
    outstandingJobs_.store(0, std::memory_order_release);
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

bool ReaderAsyncJobsController::isJobRunning() const {
  return outstandingJobs_.load(std::memory_order_acquire) != 0 ||
         currentJob_.load(std::memory_order_acquire) != JobType::None;
}

void ReaderAsyncJobsController::requestCancelCurrentJob() {
  cancelGeneration_.fetch_add(1, std::memory_order_acq_rel);
  clearQueuedCommands();
}

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

void ReaderAsyncJobsController::enqueuePendingPageTurn(const int direction, const char* reason, const int workerState) {
  navigationJob_.queuedTurn += direction > 0 ? 1 : -1;
  if (!navigationJob_.queuedTurnHasQueuedMs) {
    navigationJob_.queuedTurnQueuedMs = millis();
    navigationJob_.queuedTurnHasQueuedMs = true;
  }
  LOG_INF("RDR_NAV", "[INPUT] deferred page-turn dir=%d queue=%d reason=%s workerState=%d preemptAge=%lu", direction,
          navigationJob_.queuedTurn, reason ? reason : "unknown", workerState,
          navigationJob_.lastCachePreemptRequestedMs == 0
              ? 0UL
              : static_cast<unsigned long>(millis() - navigationJob_.lastCachePreemptRequestedMs));
}

bool ReaderAsyncJobsController::deferPageTurnUntilWorkerStops(const int direction, const bool workerRunning,
                                                              const int workerState,
                                                              const std::function<void()>& requestStop) {
  if (!workerRunning) {
    return false;
  }

  navigationJob_.deferredTurnAwaitingWorkerIdle = true;
  navigationJob_.deferredTurnIdleLogged = false;
  enqueuePendingPageTurn(direction, "background-worker-active", workerState);
  if (requestStop) {
    requestStop();
  }
  return true;
}

void ReaderAsyncJobsController::noteQueuedTurnWorkerIdle(const bool workerRunning) {
  if (workerRunning || navigationJob_.queuedTurn == 0 || !navigationJob_.deferredTurnAwaitingWorkerIdle ||
      navigationJob_.deferredTurnIdleLogged) {
    return;
  }

  navigationJob_.deferredTurnIdleLogged = true;
  const uint32_t queuedForMs = !navigationJob_.queuedTurnHasQueuedMs
                                   ? 0
                                   : static_cast<uint32_t>(millis() - navigationJob_.queuedTurnQueuedMs);
  LOG_INF("RDR_NAV", "[INPUT] deferred page-turn resumed queue=%d wait=%lu", navigationJob_.queuedTurn,
          static_cast<unsigned long>(queuedForMs));
}

bool ReaderAsyncJobsController::tryConsumeQueuedTurn(const bool workerRunning, const bool needsRender,
                                                     const bool pendingTocJump, const bool pendingPageLoad,
                                                     const bool menuMode, const bool bookmarkMode, const bool tocMode,
                                                     int& queuedTurn, uint32_t& queuedForMs) {
  queuedTurn = 0;
  queuedForMs = 0;

  if (navigationJob_.queuedTurn == 0 || needsRender || pendingTocJump || pendingPageLoad || menuMode || bookmarkMode ||
      tocMode || workerRunning) {
    return false;
  }

  queuedTurn = navigationJob_.queuedTurn > 0 ? 1 : -1;
  navigationJob_.queuedTurn -= queuedTurn;
  queuedForMs = !navigationJob_.queuedTurnHasQueuedMs
                    ? 0
                    : static_cast<uint32_t>(millis() - navigationJob_.queuedTurnQueuedMs);
  if (navigationJob_.queuedTurn == 0) {
    navigationJob_.clearQueuedTurn();
  }
  return true;
}

void ReaderAsyncJobsController::markPageLoadBlocked(const int spine, const int page,
                                                    const ReaderNavigationBlockReason reason) {
  navigationJob_.blockedReason = reason;
  navigationJob_.blockedSpine = spine;
  navigationJob_.blockedPage = page;
  navigationJob_.blockedFailures++;
  navigationJob_.blockedAtMs = millis();
  navigationJob_.clearQueuedTurn();
  LOG_INF("RDR_NAV", "[NAV] page-load blocked spine=%d page=%d reason=%u failures=%u", spine, page,
          static_cast<unsigned>(reason), static_cast<unsigned>(navigationJob_.blockedFailures));
}

void ReaderAsyncJobsController::clearPendingTocJump() {
  navigationJob_.clearTocJump();
}

void ReaderAsyncJobsController::armPendingTocJump(const int targetSpine, const std::string& anchor,
                                                  const int targetPageHint) {
  navigationJob_.clearPageLoad();
  navigationJob_.kind = ReaderNavigationJobKind::TocJump;
  navigationJob_.visibility = ReaderNavigationJobVisibility::BlockingOverlay;
  navigationJob_.tocJumpActive = true;
  navigationJob_.tocJumpIndexingShown = false;
  navigationJob_.tocJumpDeferredDisplay = false;
  navigationJob_.tocJumpTargetSpine = targetSpine;
  navigationJob_.tocJumpTargetPageHint = targetPageHint;
  navigationJob_.tocJumpAnchor = anchor;
  navigationJob_.tocJumpRetryCount = 0;
  navigationJob_.tocJumpStartedMs = millis();
  navigationJob_.tocJumpLastDiagMs = 0;
  navigationJob_.tocFirstPageReady = false;
  navigationJob_.clearBlocked();
}

void ReaderAsyncJobsController::decrementPendingTocJumpRetry() {
  if (navigationJob_.tocJumpRetryCount > 0) {
    navigationJob_.tocJumpRetryCount--;
  }
}

void ReaderAsyncJobsController::clearPendingPageLoad() {
  navigationJob_.clearPageLoad();
}

void ReaderAsyncJobsController::armPendingPageLoad(const int targetSpine, const int targetPage, const bool requireComplete,
                                                   const bool useIndexingMessage) {
  const bool sameRequest = navigationJob_.pageLoadActive && navigationJob_.pageLoadTargetSpine == targetSpine &&
                           navigationJob_.pageLoadTargetPage == targetPage &&
                           navigationJob_.pageLoadRequireComplete == requireComplete &&
                           navigationJob_.pageLoadUseIndexingMessage == useIndexingMessage;

  navigationJob_.clearTocJump();
  if (!navigationJob_.blocksPageLoad(targetSpine, targetPage)) {
    navigationJob_.clearBlocked();
  }
  navigationJob_.kind = ReaderNavigationJobKind::PageLoad;
  navigationJob_.visibility = ReaderNavigationJobVisibility::BlockingOverlay;
  navigationJob_.pageLoadActive = true;
  navigationJob_.pageLoadTargetSpine = targetSpine;
  navigationJob_.pageLoadTargetPage = targetPage;
  navigationJob_.pageLoadRequireComplete = requireComplete;
  navigationJob_.pageLoadUseIndexingMessage = useIndexingMessage;
  if (!sameRequest) {
    navigationJob_.pageLoadRetryCount = 0;
    navigationJob_.pageLoadStartedMs = millis();
    navigationJob_.pageLoadLastDiagMs = 0;
    navigationJob_.pageLoadNextRetryMs = 0;
    navigationJob_.pageLoadMessageShown = false;
  }
}

void ReaderAsyncJobsController::decrementPendingPageLoadRetry() {
  if (navigationJob_.pageLoadRetryCount > 0) {
    navigationJob_.pageLoadRetryCount--;
  }
}

bool ReaderAsyncJobsController::enqueue(const Command& cmd) {
  if (!commandQueue_) {
    return false;
  }
  if (!workerTask_.isRunning() && !startWorker()) {
    return false;
  }
  Command queuedCmd = cmd;
  // Stamp work before publishing it to the queue.  If cancellation races
  // between this load and xQueueSend(), the old generation makes the handler's
  // abort callback fire immediately; a dequeue can no longer "adopt" the new
  // generation and accidentally survive the cancellation.
  queuedCmd.generation = cancelGeneration_.load(std::memory_order_acquire);
  outstandingJobs_.fetch_add(1, std::memory_order_acq_rel);
  if (stateEvents_) xEventGroupClearBits(stateEvents_, EVENT_IDLE);
  if (xQueueSend(commandQueue_, &queuedCmd, 0) != pdTRUE) {
    LOG_ERR(TAG, "[ASYNC] command queue full type=%d", static_cast<int>(cmd.type));
    decrementOutstanding(1);
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
    decrementOutstanding(static_cast<uint16_t>(cleared));
    LOG_DBG(TAG, "[ASYNC] dropped %u stale queued command(s)", static_cast<unsigned>(cleared));
  }
  if (currentJob_.load(std::memory_order_acquire) == JobType::None &&
      outstandingJobs_.load(std::memory_order_acquire) == 0 && stateEvents_) {
    xEventGroupSetBits(stateEvents_, EVENT_IDLE);
  }
  return cleared;
}

uint16_t ReaderAsyncJobsController::decrementOutstanding(const uint16_t count) {
  uint16_t current = outstandingJobs_.load(std::memory_order_acquire);
  for (;;) {
    const uint16_t next = current > count ? static_cast<uint16_t>(current - count) : 0;
    if (outstandingJobs_.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
      return next;
    }
  }
}

ReaderAsyncJobsController::AbortCallback ReaderAsyncJobsController::abortCallback(
    const uint32_t generation) const {
  return [this, generation]() {
    if (cancelGeneration_.load(std::memory_order_acquire) != generation ||
        workerTask_.shouldStop()) {
      return true;
    }
    // Abort parsing early when heap is dangerously low to prevent std::bad_alloc.
    //
    // v2.0.157: lowered the free-heap threshold from 15 KB to 6 KB.  The old
    // 15 KB number was calibrated for the framebuffer-scratch ZIP-dict era
    // (the dict cost 0 heap, only ~16 KB of transient I/O buffers were on
    // the heap during a chapter extract).  v2.0.156 moved the dict to the
    // heap to break the framebuffer race that caused mojibake and screen
    // garbage — so a chapter extract now transiently holds ~32 KB dict +
    // ~16 KB I/O + ~5 KB uzlib state ≈ 50–55 KB.  Boot free heap is ~75 KB,
    // so free heap DURING extract sits around 10–25 KB — and the 15 KB
    // gate fired on every attempt, never letting the extract finish.
    //
    // 6 KB matches the EXTRACT/NORMALIZE phase's own `PREPARE_MIN_FREE_HEAP`
    // budget (8 KB largest block) plus a small free-bytes headroom.  The
    // gate's job is to prevent std::bad_alloc downstream — uzlib has
    // already alloc'd its dict + state by the time this fires, so no new
    // malloc happens INSIDE the decompress loop.  The only remaining
    // failure modes need <6 KB available (small string operations, LittleFS
    // write buffer top-ups), which the largestBlock guard below catches.
    const size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (freeBytes < 6 * 1024) {
      LOG_ERR(TAG, "Aborting job: heap dangerously low (%u bytes free)", static_cast<unsigned>(freeBytes));
      return true;
    }
    // Also check catastrophic heap fragmentation.  Long text now spills to
    // LittleFS and interactive page-fill must be allowed to continue with a
    // 7-8 KB largest block; aborting there creates a permanent no-page loop.
    // Only stop below 4 KB, where the next layout allocation is genuinely risky.
    const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largestBlock < 4 * 1024) {
      LOG_ERR(TAG, "Aborting job: heap fragmented (largest=%u free=%u)",
              static_cast<unsigned>(largestBlock), static_cast<unsigned>(freeBytes));
      return true;
    }
    return false;
  };
}

ReaderAsyncJobsController::AbortCallback ReaderAsyncJobsController::abortCallbackUiOnly() const {
  const uint32_t generation = currentJobGeneration_.load(std::memory_order_acquire);
  return [this, generation]() {
    return cancelGeneration_.load(std::memory_order_acquire) != generation ||
           workerTask_.shouldStop();
  };
}

void ReaderAsyncJobsController::workerLoop() {
  while (!workerTask_.shouldStop()) {
    Command cmd;
    if (xQueueReceive(commandQueue_, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
      continue;
    }

    const uint32_t jobGeneration = cmd.generation;
    currentJobGeneration_.store(jobGeneration, std::memory_order_release);
    currentJob_.store(cmd.type, std::memory_order_release);
    if (stateEvents_) {
      xEventGroupClearBits(stateEvents_, EVENT_IDLE);
    }

    const int priority = cmd.type == JobType::BackgroundCache ? 0 : kInteractiveCacheTaskPriority;
    vTaskPrioritySet(nullptr, priority);

    const AbortCallback abort = abortCallback(jobGeneration);
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
    const uint16_t remaining = decrementOutstanding(1);
    if (stateEvents_ && remaining == 0 &&
        outstandingJobs_.load(std::memory_order_acquire) == 0) {
      xEventGroupSetBits(stateEvents_, EVENT_IDLE);
    }
  }
}

}  // namespace snapix::reader
