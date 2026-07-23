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

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
