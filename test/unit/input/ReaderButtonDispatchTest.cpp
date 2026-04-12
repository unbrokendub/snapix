#include "test_utils.h"

#include <chrono>
#include <thread>

#include "states/reader/ReaderNavigationController.h"

int main() {
  TestUtils::TestRunner runner("ReaderNavigationController");

  using papyrix::reader::ReaderNavigationController;

  {
    ReaderNavigationController controller;
    controller.setHoldNavigated(true);
    controller.setPowerPressStartedMs(42);
    controller.markCachePreemptRequested(77);
    controller.enqueuePendingPageTurn(1, "test", 2);
    controller.resetSession();

    runner.expectFalse(controller.holdNavigated(), "reset clears hold flag");
    runner.expectEq(uint32_t(0), controller.powerPressStartedMs(), "reset clears power press timestamp");
    runner.expectEq(int(0), controller.queuedPendingPageTurnRef(), "reset clears queued turns");
    runner.expectEq(uint32_t(0), controller.queuedPendingPageTurnQueuedMsRef(), "reset clears queued turn age");
    runner.expectEq(uint32_t(0), controller.lastCachePreemptRequestedMs(), "reset clears preempt timestamp");
  }

  {
    ReaderNavigationController controller;
    bool stopRequested = false;
    const bool deferred = controller.deferPageTurnUntilWorkerStops(
        1, true, 3, [&]() { stopRequested = true; });

    runner.expectTrue(deferred, "defer queues turn while worker is active");
    runner.expectTrue(stopRequested, "defer requests cooperative stop");
    runner.expectEq(int(1), controller.queuedPendingPageTurnRef(), "queued turn direction stored");
  }

  {
    ReaderNavigationController controller;
    bool stopRequested = false;
    const bool deferred = controller.deferPageTurnUntilWorkerStops(
        -1, false, 0, [&]() { stopRequested = true; });

    runner.expectFalse(deferred, "defer ignored when worker is already idle");
    runner.expectFalse(stopRequested, "idle path does not request stop");
    runner.expectEq(int(0), controller.queuedPendingPageTurnRef(), "idle path leaves queue untouched");
  }

  {
    ReaderNavigationController controller;
    controller.markCachePreemptRequested(millis());
    controller.enqueuePendingPageTurn(1, "first", 2);
    controller.enqueuePendingPageTurn(1, "second", 2);

    int queuedTurn = 0;
    uint32_t queuedForMs = 0;
    const bool consumed = controller.tryConsumeQueuedTurn(false, false, false, false, false, false, false, queuedTurn,
                                                          queuedForMs);

    runner.expectTrue(consumed, "queued turn consumed when worker and overlays are idle");
    runner.expectEq(int(1), queuedTurn, "consume returns one step at a time");
    runner.expectEq(int(1), controller.queuedPendingPageTurnRef(), "remaining queued turn stays pending");
    runner.expectTrue(queuedForMs >= 0, "queue age is reported");
  }

  {
    ReaderNavigationController controller;
    controller.enqueuePendingPageTurn(-1, "overlay-block", 2);

    int queuedTurn = 0;
    uint32_t queuedForMs = 0;
    const bool consumed = controller.tryConsumeQueuedTurn(
        false, false, false, false, false, true, false, queuedTurn, queuedForMs);

    runner.expectFalse(consumed, "overlay blocks queued turn consumption");
    runner.expectEq(int(0), queuedTurn, "blocked consume does not emit direction");
    runner.expectEq(int(-1), controller.queuedPendingPageTurnRef(), "blocked consume keeps queue intact");
  }

  {
    ReaderNavigationController controller;
    controller.markCachePreemptRequested(millis());
    controller.enqueuePendingPageTurn(1, "single", 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    int queuedTurn = 0;
    uint32_t queuedForMs = 0;
    const bool consumed = controller.tryConsumeQueuedTurn(false, false, false, false, false, false, false, queuedTurn,
                                                          queuedForMs);

    runner.expectTrue(consumed, "single queued turn is consumed");
    runner.expectEq(int(1), queuedTurn, "single queued turn keeps direction");
    runner.expectEq(int(0), controller.queuedPendingPageTurnRef(), "queue becomes empty after final consume");
    runner.expectEq(uint32_t(0), controller.lastCachePreemptRequestedMs(), "preempt timestamp resets after queue drains");
    runner.expectTrue(queuedForMs >= 1, "queue age uses original enqueue time");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
