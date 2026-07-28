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
    navigation.setPowerPressActive(true);
    navigation.setPowerPressStartedMs(42);
    jobs.markCachePreemptRequested(77);
    jobs.enqueuePendingPageTurn(1, "test", 2);
    navigation.resetSession();
    jobs.clearQueuedPageTurns();

    runner.expectFalse(navigation.holdNavigated(), "reset clears hold flag");
    runner.expectFalse(navigation.powerPressActive(), "reset clears power press state");
    runner.expectEq(uint32_t(0), navigation.powerPressStartedMs(), "reset clears power press timestamp");
    runner.expectEq(int(0), jobs.queuedPendingPageTurnRef(), "reset clears queued turns");
    runner.expectEq(uint32_t(0), jobs.queuedPendingPageTurnQueuedMsRef(), "reset clears queued turn age");
    runner.expectEq(uint32_t(0), jobs.lastCachePreemptRequestedMsRef(), "reset clears preempt timestamp");
  }

  {
    const snapix::Event press =
        snapix::Event::buttonPress(snapix::Button::Power, 100);
    const snapix::Event release =
        snapix::Event::buttonRelease(snapix::Button::Power, 220);

    runner.expectEq(uint32_t(100), press.timestampMs,
                    "power press keeps hardware observation time");
    runner.expectTrue(
        snapix::reader::isShortPowerRelease(release, true, press.timestampMs,
                                            1200, 400),
        "delayed main-loop dispatch does not turn a short press into a long press");
  }

  {
    const snapix::Event release =
        snapix::Event::buttonRelease(snapix::Button::Power, 550);
    runner.expectFalse(
        snapix::reader::isShortPowerRelease(release, true, 100, 2000, 400),
        "real long power hold remains a long press despite delayed dispatch");
    runner.expectFalse(
        snapix::reader::isShortPowerRelease(release, false, 100, 2000, 400),
        "release without an active power press is ignored");
  }

  {
    runner.expectTrue(
        snapix::reader::shouldPrioritizeNextSectionPrefetch(
            true, true, false, false, false, true),
        "EPUB/FB2 read-ahead prepares next section while current has runway");
    runner.expectFalse(
        snapix::reader::shouldPrioritizeNextSectionPrefetch(
            true, true, true, false, false, true),
        "active section near tail remains the immediate cache priority");
    runner.expectFalse(
        snapix::reader::shouldPrioritizeNextSectionPrefetch(
            true, true, false, true, false, true),
        "read-ahead stops once next section has a readable page");
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
    const bool consumed = controller.tryConsumeQueuedTurns(false, false, false, false, false, false, false, queuedTurn,
                                                           queuedForMs);

    runner.expectTrue(consumed, "queued turns consumed when worker and overlays are idle");
    runner.expectEq(int(2), queuedTurn, "consume returns the complete net delta");
    runner.expectEq(int(0), controller.queuedPendingPageTurnRef(), "batched queue is drained atomically");
    runner.expectTrue(queuedForMs >= 0, "queue age is reported");
  }

  {
    ReaderAsyncJobsController controller;
    controller.enqueuePendingPageTurn(-1, "overlay-block", 2);

    int queuedTurn = 0;
    uint32_t queuedForMs = 0;
    const bool consumed = controller.tryConsumeQueuedTurns(
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
    const bool consumed = controller.tryConsumeQueuedTurns(false, false, false, false, false, false, false, queuedTurn,
                                                           queuedForMs);

    runner.expectTrue(consumed, "single queued turn is consumed");
    runner.expectEq(int(1), queuedTurn, "single queued turn keeps direction");
    runner.expectEq(int(0), controller.queuedPendingPageTurnRef(), "queue becomes empty after final consume");
    runner.expectEq(uint32_t(0), controller.lastCachePreemptRequestedMsRef(),
                    "preempt timestamp resets after queue drains");
    runner.expectTrue(queuedForMs >= 1, "queue age uses original enqueue time");
  }

  {
    ReaderAsyncJobsController controller;
    controller.markCachePreemptRequested(42);
    controller.enqueuePendingPageTurn(1, "forward", 2);
    controller.enqueuePendingPageTurn(-1, "reverse-cancels", 2);

    runner.expectEq(int(0), controller.queuedPendingPageTurnRef(), "opposite clicks cancel as a net delta");
    runner.expectEq(uint32_t(0), controller.queuedPendingPageTurnQueuedMsRef(),
                    "cancelled queue clears its stale age");
    runner.expectEq(uint32_t(0), controller.lastCachePreemptRequestedMsRef(),
                    "cancelled queue clears stale preemption state");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
