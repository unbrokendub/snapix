#include "ReaderNavigationController.h"

#include <Arduino.h>
#include <Logging.h>

#define TAG "RDR_NAV"

namespace papyrix::reader {

void ReaderNavigationController::resetSession() {
  holdNavigated_ = false;
  powerPressStartedMs_ = 0;
  queuedPendingEpubTurn_ = 0;
  queuedPendingEpubTurnQueuedMs_ = 0;
  lastCachePreemptRequestedMs_ = 0;
  deferredTurnAwaitingWorkerIdle_ = false;
  deferredTurnIdleLogged_ = false;
}

void ReaderNavigationController::enqueuePendingPageTurn(const int direction, const char* reason, const int workerState) {
  queuedPendingEpubTurn_ += direction > 0 ? 1 : -1;
  if (queuedPendingEpubTurnQueuedMs_ == 0) {
    queuedPendingEpubTurnQueuedMs_ = millis();
  }
  LOG_INF(TAG, "[INPUT] deferred page-turn dir=%d queue=%d reason=%s workerState=%d preemptAge=%lu", direction,
          queuedPendingEpubTurn_, reason ? reason : "unknown", workerState,
          lastCachePreemptRequestedMs_ == 0 ? 0UL : static_cast<unsigned long>(millis() - lastCachePreemptRequestedMs_));
}

bool ReaderNavigationController::deferPageTurnUntilWorkerStops(const int direction, const bool workerRunning,
                                                               const int workerState,
                                                               const std::function<void()>& requestStop) {
  if (!workerRunning) {
    return false;
  }

  deferredTurnAwaitingWorkerIdle_ = true;
  deferredTurnIdleLogged_ = false;
  enqueuePendingPageTurn(direction, "background-worker-active", workerState);
  if (requestStop) {
    requestStop();
  }
  return true;
}

void ReaderNavigationController::noteWorkerIdle(const bool workerRunning) {
  if (workerRunning || queuedPendingEpubTurn_ == 0 || !deferredTurnAwaitingWorkerIdle_ || deferredTurnIdleLogged_) {
    return;
  }

  deferredTurnIdleLogged_ = true;
  const uint32_t queuedForMs =
      queuedPendingEpubTurnQueuedMs_ == 0 ? 0 : static_cast<uint32_t>(millis() - queuedPendingEpubTurnQueuedMs_);
  LOG_INF(TAG, "[INPUT] deferred page-turn resumed queue=%d wait=%lu", queuedPendingEpubTurn_,
          static_cast<unsigned long>(queuedForMs));
}

bool ReaderNavigationController::tryConsumeQueuedTurn(const bool workerRunning, const bool needsRender,
                                                      const bool pendingTocJump, const bool pendingPageLoad,
                                                      const bool menuMode, const bool bookmarkMode, const bool tocMode,
                                                      int& queuedTurn, uint32_t& queuedForMs) {
  queuedTurn = 0;
  queuedForMs = 0;

  if (queuedPendingEpubTurn_ == 0 || needsRender || pendingTocJump || pendingPageLoad || menuMode || bookmarkMode ||
      tocMode || workerRunning) {
    return false;
  }

  queuedTurn = queuedPendingEpubTurn_ > 0 ? 1 : -1;
  queuedPendingEpubTurn_ -= queuedTurn;
  queuedForMs =
      queuedPendingEpubTurnQueuedMs_ == 0 ? 0 : static_cast<uint32_t>(millis() - queuedPendingEpubTurnQueuedMs_);
  if (queuedPendingEpubTurn_ == 0) {
    queuedPendingEpubTurnQueuedMs_ = 0;
    lastCachePreemptRequestedMs_ = 0;
    deferredTurnAwaitingWorkerIdle_ = false;
    deferredTurnIdleLogged_ = false;
  }
  return true;
}

}  // namespace papyrix::reader
