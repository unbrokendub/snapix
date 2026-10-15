#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "freertos/queue.h"
#include "freertos/task.h"
#include "states/reader/ReaderAsyncJobsController.h"

int main() {
  TestUtils::TestRunner runner("ReaderAsyncJobsController");

  using snapix::reader::BackgroundCacheWakeReason;
  using snapix::reader::ReaderAsyncJobsController;

  {
    cleanupMockTasks();
    cleanupMockQueues();

    std::atomic<int> runCount{0};

    {
      ReaderAsyncJobsController controller;

      controller.setBackgroundCacheHandler([&](const ReaderAsyncJobsController::BackgroundCacheRequest&,
                                               const ReaderAsyncJobsController::AbortCallback&) {
        runCount.fetch_add(1);
      });

      runner.expectTrue(controller.startWorker(), "startWorker succeeds");

      ReaderAsyncJobsController::BackgroundCacheRequest request;
      request.plan.shouldStart = true;
      request.plan.reason = BackgroundCacheWakeReason::CurrentCachePartial;
      request.plan.candidateSpine = 3;

      runner.expectTrue(controller.queueBackgroundCache(request), "background cache request queues");
      runner.expectTrue(controller.isJobRunning(), "queued work is reported before handler starts");
      runner.expectTrue(controller.waitUntilIdle(1000), "worker returns to idle after job");
      runner.expectEq(int(1), runCount.load(), "queued background job executes exactly once");

      runner.expectTrue(controller.stopWorker(), "stopWorker succeeds after idle job");
    }
    cleanupMockTasks();
    cleanupMockQueues();
  }

  {
    cleanupMockTasks();
    cleanupMockQueues();

    std::atomic<int> runCount{0};

    {
      ReaderAsyncJobsController controller;

      controller.setBackgroundCacheHandler([&](const ReaderAsyncJobsController::BackgroundCacheRequest&,
                                               const ReaderAsyncJobsController::AbortCallback&) {
        runCount.fetch_add(1);
      });

      runner.expectTrue(controller.startWorker(), "restart scenario starts");

      ReaderAsyncJobsController::BackgroundCacheRequest request;
      request.plan.shouldStart = true;
      request.plan.reason = BackgroundCacheWakeReason::CurrentCachePartial;
      runner.expectTrue(controller.queueBackgroundCache(request), "restart scenario queues request");
      runner.expectTrue(controller.stopWorker(), "restart scenario stops worker");
      const int runCountAfterStop = runCount.load();

      controller.setBackgroundCacheHandler([&](const ReaderAsyncJobsController::BackgroundCacheRequest&,
                                               const ReaderAsyncJobsController::AbortCallback&) {
        runCount.fetch_add(100);
      });

      runner.expectTrue(controller.startWorker(), "worker restarts cleanly");
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      runner.expectEq(runCountAfterStop, runCount.load(), "stale queued commands do not survive worker restart");

      controller.stopWorker();
    }
    cleanupMockTasks();
    cleanupMockQueues();
  }

  {
    cleanupMockTasks();
    cleanupMockQueues();

    std::atomic<bool> sawAbort{false};

    {
      ReaderAsyncJobsController controller;

      controller.setPageFillHandler([&](const ReaderAsyncJobsController::PageFillRequest&,
                                        const ReaderAsyncJobsController::AbortCallback& shouldAbort) {
        while (!shouldAbort()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        sawAbort.store(true);
      });

      runner.expectTrue(controller.startWorker(), "cancel scenario starts");

      ReaderAsyncJobsController::PageFillRequest request;
      request.targetSpine = 2;
      request.targetPage = 14;
      runner.expectTrue(controller.queuePageFillWork(request), "page fill request queues");

      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      controller.requestCancelCurrentJob();
      runner.expectTrue(controller.stopWorker(), "stopWorker cancels running job");
      runner.expectTrue(sawAbort.load(), "abort callback is observed by running handler");
    }

    cleanupMockTasks();
    cleanupMockQueues();
  }

  {
    cleanupMockTasks();
    cleanupMockQueues();

    std::atomic<int> started{0};
    std::atomic<int> completed{0};

    {
      ReaderAsyncJobsController controller;
      controller.setPageFillHandler(
          [&](const ReaderAsyncJobsController::PageFillRequest&,
              const ReaderAsyncJobsController::AbortCallback& shouldAbort) {
            started.fetch_add(1);
            while (!shouldAbort()) {
              std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            completed.fetch_add(1);
          });

      runner.expectTrue(controller.startWorker(), "queued-cancel scenario starts");
      ReaderAsyncJobsController::PageFillRequest request;
      runner.expectTrue(controller.queuePageFillWork(request), "first cancellable job queues");
      runner.expectTrue(controller.queuePageFillWork(request), "second cancellable job queues");
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      controller.requestCancelCurrentJob();
      runner.expectTrue(controller.waitUntilIdle(1000), "cancel drains active and queued work");
      runner.expectEq(1, started.load(), "queued job is removed instead of executing");
      runner.expectEq(1, completed.load(), "active job observes cancellation");
      runner.expectFalse(controller.isJobRunning(), "controller reports idle after queue drain");

      controller.setPageFillHandler(
          [&](const ReaderAsyncJobsController::PageFillRequest&,
              const ReaderAsyncJobsController::AbortCallback&) { completed.fetch_add(10); });
      runner.expectTrue(controller.queuePageFillWork(request), "new job queues after cancellation");
      runner.expectTrue(controller.waitUntilIdle(1000), "new generation completes normally");
      runner.expectEq(11, completed.load(), "cancellation does not poison future jobs");
      controller.stopWorker();
    }

    cleanupMockTasks();
    cleanupMockQueues();
  }

  {
    ReaderAsyncJobsController controller;
    auto& refresh = controller.pendingRefresh();
    refresh.publish(4, 9);
    int spine = -1;
    int page = -1;
    uint32_t oldToken = 0;
    runner.expectTrue(refresh.snapshot(spine, page, oldToken), "refresh snapshot is available");
    runner.expectEq(4, spine, "refresh publishes spine and page together");
    runner.expectEq(9, page, "refresh page matches snapshot");

    refresh.publish(7, 3);
    runner.expectFalse(refresh.clearIfUnchanged(oldToken), "old consumer cannot erase newer refresh");
    uint32_t newToken = 0;
    runner.expectTrue(refresh.snapshot(spine, page, newToken), "newer refresh remains pending");
    runner.expectEq(7, spine, "newer refresh spine survives stale clear");
    runner.expectEq(3, page, "newer refresh page survives stale clear");
    runner.expectTrue(refresh.clearIfUnchanged(newToken), "current refresh clears by token");
  }

  // ---- v3.11.12: DeferredJobSlot protocol (UI ↔ ReaderAsync hand-off) ----
  {
    using snapix::reader::DeferredJobSlot;
    using S = DeferredJobSlot::State;
    DeferredJobSlot slot;
    runner.expectTrue(slot.state() == S::Idle, "slot starts idle");
    runner.expectFalse(slot.begin(), "worker cannot claim an idle slot");
    runner.expectTrue(slot.request(), "UI requests work");
    runner.expectFalse(slot.request(), "second request is a no-op");
    runner.expectFalse(slot.begin(), "worker cannot claim work that was never queued");
    runner.expectTrue(slot.markQueued(), "UI marks queued before sending");
    runner.expectFalse(slot.recoverDropped(true), "queued work with a busy worker is not 'dropped'");
    runner.expectTrue(slot.recoverDropped(false), "queued work with an idle worker was dropped by a cancel");
    runner.expectTrue(slot.state() == S::Pending, "dropped work is pending again");
    runner.expectTrue(slot.markQueued() && slot.begin(), "requeued work is claimed");
    runner.expectTrue(slot.consume() == S::Idle, "nothing to consume while running");
    slot.finish(S::Queued);  // anything but Done/Failed means "retry"
    runner.expectTrue(slot.state() == S::Pending, "aborted work goes back to pending");
    runner.expectTrue(slot.markQueued() && slot.begin(), "retry is claimed");
    slot.finish(S::Done);
    runner.expectTrue(slot.consume() == S::Done, "UI consumes the outcome once");
    runner.expectTrue(slot.state() == S::Idle && slot.consume() == S::Idle, "consumed slot is idle");
    runner.expectTrue(slot.request() && slot.markQueued() && slot.begin(), "next job");
    slot.finish(S::Failed);
    runner.expectTrue(slot.consume() == S::Failed, "failure is reported");
    slot.request();
    slot.reset();
    runner.expectTrue(slot.state() == S::Idle, "reset (worker stopped) clears any state");
  }

  // ---- v3.11.12: aux jobs run on the worker; a cancel drops a queued one ----
  {
    cleanupMockTasks();
    cleanupMockQueues();

    using snapix::reader::DeferredJobSlot;
    using S = DeferredJobSlot::State;
    DeferredJobSlot slot;
    std::atomic<int> auxRuns{0};
    std::atomic<bool> fillStarted{false};
    std::atomic<int> auxKindSeen{0};

    {
      ReaderAsyncJobsController controller;
      controller.setAuxHandler([&](const ReaderAsyncJobsController::AuxRequest& request,
                                   const ReaderAsyncJobsController::AbortCallback& shouldAbort) {
        auxKindSeen.store(static_cast<int>(request.kind));
        if (!slot.begin()) return;
        auxRuns.fetch_add(1);
        slot.finish(shouldAbort() ? S::Pending : S::Done);
      });
      controller.setPageFillHandler([&](const ReaderAsyncJobsController::PageFillRequest&,
                                        const ReaderAsyncJobsController::AbortCallback& shouldAbort) {
        fillStarted.store(true);
        while (!shouldAbort()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      });
      runner.expectTrue(controller.startWorker(), "aux scenario starts");

      ReaderAsyncJobsController::AuxRequest aux;
      aux.kind = ReaderAsyncJobsController::AuxJobKind::CoverImage;

      // 1. Behind a running cancellable job, then a cancel: never runs.
      ReaderAsyncJobsController::PageFillRequest fill;
      runner.expectTrue(controller.queuePageFillWork(fill), "blocking job queues");
      for (int i = 0; i < 100 && !fillStarted.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      slot.request();
      runner.expectTrue(slot.markQueued() && controller.queueAuxWork(aux), "aux job queues behind it");
      controller.requestCancelCurrentJob();
      runner.expectTrue(controller.waitUntilIdle(1000), "cancel drains the worker");
      runner.expectEq(0, auxRuns.load(), "queued aux job dropped by the cancel");
      runner.expectTrue(slot.recoverDropped(controller.isJobRunning()), "UI recovers the dropped job");

      // 2. Requeued on an idle worker: runs once and reports Done.
      runner.expectTrue(slot.markQueued() && controller.queueAuxWork(aux), "aux job requeued");
      runner.expectTrue(controller.waitUntilIdle(1000), "aux job completes");
      runner.expectEq(1, auxRuns.load(), "aux job ran exactly once");
      runner.expectEq(static_cast<int>(ReaderAsyncJobsController::AuxJobKind::CoverImage), auxKindSeen.load(),
                      "aux request kind reaches the handler");
      runner.expectTrue(slot.consume() == S::Done, "UI sees the result after idle");
      controller.stopWorker();
    }
    cleanupMockTasks();
    cleanupMockQueues();
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
