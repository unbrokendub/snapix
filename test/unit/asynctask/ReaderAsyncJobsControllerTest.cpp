#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "freertos/queue.h"
#include "freertos/task.h"
#include "states/reader/ReaderAsyncJobsController.h"

int main() {
  TestUtils::TestRunner runner("ReaderAsyncJobsController");

  using papyrix::reader::BackgroundCacheWakeReason;
  using papyrix::reader::ReaderAsyncJobsController;

  {
    cleanupMockTasks();
    cleanupMockQueues();

    ReaderAsyncJobsController controller;
    std::atomic<int> runCount{0};

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
    runner.expectTrue(controller.waitUntilIdle(1000), "worker returns to idle after job");
    runner.expectEq(int(1), runCount.load(), "queued background job executes exactly once");

    runner.expectTrue(controller.stopWorker(), "stopWorker succeeds after idle job");
    cleanupMockTasks();
    cleanupMockQueues();
  }

  {
    cleanupMockTasks();
    cleanupMockQueues();

    ReaderAsyncJobsController controller;
    std::atomic<int> runCount{0};

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

    controller.setBackgroundCacheHandler([&](const ReaderAsyncJobsController::BackgroundCacheRequest&,
                                             const ReaderAsyncJobsController::AbortCallback&) {
      runCount.fetch_add(100);
    });

    runner.expectTrue(controller.startWorker(), "worker restarts cleanly");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    runner.expectEq(int(0), runCount.load(), "stale queued commands do not survive worker restart");

    controller.stopWorker();
    cleanupMockTasks();
    cleanupMockQueues();
  }

  {
    cleanupMockTasks();
    cleanupMockQueues();

    ReaderAsyncJobsController controller;
    std::atomic<bool> sawAbort{false};

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

    cleanupMockTasks();
    cleanupMockQueues();
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
