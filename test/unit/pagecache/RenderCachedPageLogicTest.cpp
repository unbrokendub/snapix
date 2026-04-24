#include "test_utils.h"

#include "GfxRenderer.h"
#include "states/reader/ReaderDocumentResources.h"

int main() {
  TestUtils::TestRunner runner("ReaderDocumentResources");

  GfxRenderer renderer;
  snapix::reader::ReaderDocumentResources resources(renderer);

  {
    auto foreground = resources.acquireForeground("foreground-test");
    runner.expectTrue(static_cast<bool>(foreground), "foreground session acquires idle resources");
    runner.expectTrue(resources.isForegroundOwned(), "resources report foreground owner");

    foreground.state().parserSpineIndex = 7;
    runner.expectEq(7, resources.unsafeState().parserSpineIndex, "foreground session updates shared state");

    auto worker = resources.acquireWorker("worker-denied");
    runner.expectFalse(static_cast<bool>(worker), "worker acquire denied while foreground owns resources");
  }

  runner.expectTrue(resources.isIdle(), "resources return to idle after foreground release");

  {
    auto worker = resources.acquireWorker("worker-test");
    runner.expectTrue(static_cast<bool>(worker), "worker session acquires idle resources");
    runner.expectTrue(resources.isWorkerOwned(), "resources report worker owner");

    auto moved = std::move(worker);
    runner.expectTrue(static_cast<bool>(moved), "moved session stays valid");
    runner.expectFalse(static_cast<bool>(worker), "moved-from session becomes empty");

    auto foreground = resources.acquireForeground("foreground-denied");
    runner.expectFalse(static_cast<bool>(foreground), "foreground acquire denied while worker owns resources");
  }

  runner.expectTrue(resources.isIdle(), "resources return to idle after worker release");

  {
    auto foreground = resources.acquireForeground("reacquire");
    runner.expectTrue(static_cast<bool>(foreground), "foreground reacquires after worker release");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
