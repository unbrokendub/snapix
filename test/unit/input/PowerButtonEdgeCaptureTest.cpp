#include "test_utils.h"

#include "drivers/PowerButtonEdgeCapture.h"

using snapix::drivers::PowerButtonEdge;
using snapix::drivers::PowerButtonEdgeCapture;

int main() {
  TestUtils::TestRunner runner("PowerButtonEdgeCaptureTest");

  // A complete click remains ordered even if the main loop does not poll
  // between its press and release edges.
  {
    PowerButtonEdgeCapture<8> capture;
    capture.reset(false, 100000);
    runner.expectTrue(capture.captureFromIsr(true, 110000), "capture press during blocked loop");
    runner.expectTrue(capture.captureFromIsr(false, 180000), "capture release during blocked loop");

    PowerButtonEdge edge;
    runner.expectTrue(capture.pop(edge), "pop captured press");
    runner.expectTrue(edge.pressed, "first edge is press");
    runner.expectEq(110000U, edge.timestampUs, "press keeps ISR timestamp");
    runner.expectTrue(capture.pop(edge), "pop captured release");
    runner.expectFalse(edge.pressed, "second edge is release");
    runner.expectEq(180000U, edge.timestampUs, "release keeps ISR timestamp");
    runner.expectFalse(capture.pop(edge), "buffer empty after complete click");
  }

  // Contact bounce must not turn one physical click into extra events.
  {
    PowerButtonEdgeCapture<8> capture;
    capture.reset(false, 200000);
    runner.expectTrue(capture.captureFromIsr(true, 210000), "accept initial falling edge");
    runner.expectFalse(capture.captureFromIsr(false, 212000), "reject release bounce inside 5ms");
    runner.expectFalse(capture.captureFromIsr(true, 214000), "ignore return to accepted press state");
    runner.expectTrue(capture.captureFromIsr(false, 260000), "accept real release");

    PowerButtonEdge edge;
    runner.expectTrue(capture.pop(edge) && edge.pressed, "debounced sequence contains press");
    runner.expectTrue(capture.pop(edge) && !edge.pressed, "debounced sequence contains release");
    runner.expectFalse(capture.pop(edge), "debounced sequence has no extra edges");
  }

  // Unsigned elapsed-time arithmetic keeps debounce correct across micros()
  // rollover (about every 71 minutes for the stored low 32 bits).
  {
    PowerButtonEdgeCapture<8> capture;
    capture.reset(false, 0xFFFFFF00U);
    runner.expectTrue(capture.captureFromIsr(true, 0xFFFFFF80U), "first rollover-near edge accepted");
    runner.expectFalse(capture.captureFromIsr(false, 0x00000100U), "rollover bounce rejected");
    runner.expectTrue(capture.captureFromIsr(false, 0x00002000U), "rollover release accepted after debounce");
  }

  // Capacity is bounded in ISR context; overflow is explicit rather than
  // overwriting an earlier press or release.
  {
    PowerButtonEdgeCapture<4> capture;  // Three usable slots.
    capture.reset(false, 0);
    runner.expectTrue(capture.captureFromIsr(true, 10000), "overflow test edge 1");
    runner.expectTrue(capture.captureFromIsr(false, 20000), "overflow test edge 2");
    runner.expectTrue(capture.captureFromIsr(true, 30000), "overflow test edge 3");
    runner.expectFalse(capture.captureFromIsr(false, 40000), "full buffer rejects newest edge");
    runner.expectEq(1U, capture.droppedCount(), "overflow counted");

    PowerButtonEdge edge;
    runner.expectTrue(capture.pop(edge) && edge.pressed, "overflow preserves oldest edge 1");
    runner.expectTrue(capture.pop(edge) && !edge.pressed, "overflow preserves oldest edge 2");
    runner.expectTrue(capture.pop(edge) && edge.pressed, "overflow preserves oldest edge 3");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
