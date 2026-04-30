#pragma once

#include <cstdint>
#include <functional>

namespace snapix::reader {

class ReaderNavigationController {
 public:
  void resetSession();

  bool& holdNavigatedRef() { return holdNavigated_; }
  uint32_t& powerPressStartedMsRef() { return powerPressStartedMs_; }
  int& queuedPendingPageTurnRef() { return queuedPendingEpubTurn_; }
  uint32_t& queuedPendingPageTurnQueuedMsRef() { return queuedPendingEpubTurnQueuedMs_; }
  uint32_t& lastCachePreemptRequestedMsRef() { return lastCachePreemptRequestedMs_; }

  bool holdNavigated() const { return holdNavigated_; }
  void setHoldNavigated(bool value) { holdNavigated_ = value; }

  uint32_t powerPressStartedMs() const { return powerPressStartedMs_; }
  void setPowerPressStartedMs(uint32_t value) { powerPressStartedMs_ = value; }

  void markCachePreemptRequested(uint32_t nowMs) { lastCachePreemptRequestedMs_ = nowMs; }
  uint32_t lastCachePreemptRequestedMs() const { return lastCachePreemptRequestedMs_; }
  void clearCachePreemptRequested() { lastCachePreemptRequestedMs_ = 0; }

  void enqueuePendingPageTurn(int direction, const char* reason, int workerState);
  bool deferPageTurnUntilWorkerStops(int direction, bool workerRunning, int workerState,
                                     const std::function<void()>& requestStop);
  void noteWorkerIdle(bool workerRunning);
  bool tryConsumeQueuedTurn(bool workerRunning, bool needsRender, bool pendingTocJump, bool pendingPageLoad,
                            bool menuMode, bool bookmarkMode, bool tocMode, int& queuedTurn, uint32_t& queuedForMs);

 private:
  bool holdNavigated_ = false;
  uint32_t powerPressStartedMs_ = 0;
  int queuedPendingEpubTurn_ = 0;
  uint32_t queuedPendingEpubTurnQueuedMs_ = 0;
  bool queuedPendingEpubTurnHasQueuedMs_ = false;
  uint32_t lastCachePreemptRequestedMs_ = 0;
  bool deferredTurnAwaitingWorkerIdle_ = false;
  bool deferredTurnIdleLogged_ = false;
};

}  // namespace snapix::reader
