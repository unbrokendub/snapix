#include "test_utils.h"

#include <chrono>
#include <thread>

#include "platform_stubs.h"
#include "states/reader/ReaderSupport.h"
#include "states/reader/ReaderAsyncJobsController.h"
#include "states/reader/ReaderNavigationController.h"

int main() {
  TestUtils::TestRunner runner("ReaderNavigationController");

  using snapix::reader::ReaderNavigationController;
  using snapix::reader::ReaderAsyncJobsController;

  {
    runner.expectFalse(
        snapix::reader::cancelsDeferredTocFollowup(
            snapix::Event::buttonRelease(snapix::Button::Center)),
        "TOC selection release does not cancel exact-anchor follow-up");
    runner.expectFalse(
        snapix::reader::cancelsDeferredTocFollowup(
            snapix::Event::system(snapix::EventType::BatteryLow)),
        "system event does not cancel exact-anchor follow-up");
    runner.expectTrue(
        snapix::reader::cancelsDeferredTocFollowup(
            snapix::Event::buttonPress(snapix::Button::Center)),
        "new center press cancels exact-anchor follow-up");
    runner.expectTrue(
        snapix::reader::cancelsDeferredTocFollowup(
            snapix::Event::buttonRelease(snapix::Button::Right)),
        "page-turn release cancels exact-anchor follow-up");
    runner.expectTrue(
        snapix::reader::cancelsDeferredTocFollowup(
            snapix::Event::buttonRepeat(snapix::Button::Down)),
        "chapter-navigation repeat cancels exact-anchor follow-up");
  }

  {
    ReaderNavigationController navigation;
    ReaderAsyncJobsController jobs;
    navigation.setHoldNavigated(true);
    navigation.setPowerPressStartedMs(42);
    jobs.markCachePreemptRequested(77);
    jobs.enqueuePendingPageTurn(1, "test", 2);
    navigation.resetSession();
    jobs.clearQueuedPageTurns();

    runner.expectFalse(navigation.holdNavigated(), "reset clears hold flag");
    runner.expectEq(uint32_t(0), navigation.powerPressStartedMs(), "reset clears power press timestamp");
    runner.expectEq(int(0), jobs.queuedPendingPageTurnRef(), "reset clears queued turns");
    runner.expectEq(uint32_t(0), jobs.queuedPendingPageTurnQueuedMsRef(), "reset clears queued turn age");
    runner.expectEq(uint32_t(0), jobs.lastCachePreemptRequestedMsRef(), "reset clears preempt timestamp");
  }

  {
    ReaderAsyncJobsController controller;
    bool stopRequested = false;
    const bool deferred = controller.deferPageTurnUntilWorkerStops(
        1, true, 3, [&]() { stopRequested = true; });

    runner.expectTrue(deferred, "defer queues turn while worker is active");
    runner.expectTrue(stopRequested, "defer requests cooperative stop");
    runner.expectEq(int(1), controller.queuedPendingPageTurnRef(), "queued turn direction stored");
  }

  {
    ReaderAsyncJobsController controller;
    bool stopRequested = false;
    const bool deferred = controller.deferPageTurnUntilWorkerStops(
        -1, false, 0, [&]() { stopRequested = true; });

    runner.expectFalse(deferred, "defer ignored when worker is already idle");
    runner.expectFalse(stopRequested, "idle path does not request stop");
    runner.expectEq(int(0), controller.queuedPendingPageTurnRef(), "idle path leaves queue untouched");
  }

  {
    ReaderAsyncJobsController controller;
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
    ReaderAsyncJobsController controller;
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
    ReaderAsyncJobsController controller;
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
    runner.expectEq(uint32_t(0), controller.lastCachePreemptRequestedMsRef(),
                    "preempt timestamp resets after queue drains");
    runner.expectTrue(queuedForMs >= 1, "queue age uses original enqueue time");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
