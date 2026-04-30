#include "test_utils.h"

#include "drivers/Cpu.h"

int main() {
  TestUtils::TestRunner runner("CpuDriverTest");

  extern uint32_t g_mockCpuFreqMhz;

  // === Default state: not throttled ===
  {
    snapix::drivers::Cpu cpu;
    runner.expectTrue(!cpu.isThrottled(), "default: not throttled");
    runner.expectEq(uint8_t(2), cpu.loopDelayMs(), "default: active loop delay 2ms");
  }

  // === throttle() drops frequency and changes delay ===
  {
    snapix::drivers::Cpu cpu;
    g_mockCpuFreqMhz = 160;

    cpu.throttle();

    runner.expectTrue(cpu.isThrottled(), "after throttle: isThrottled true");
    runner.expectEq(uint32_t(10), g_mockCpuFreqMhz, "after throttle: freq is 10 MHz");
    runner.expectEq(uint8_t(20), cpu.loopDelayMs(), "after throttle: idle loop delay 20ms");
  }

  // === unthrottle() restores frequency ===
  {
    snapix::drivers::Cpu cpu;
    g_mockCpuFreqMhz = 160;

    cpu.throttle();
    cpu.unthrottle();

    runner.expectTrue(!cpu.isThrottled(), "after unthrottle: isThrottled false");
    runner.expectEq(uint32_t(160), g_mockCpuFreqMhz, "after unthrottle: freq is 160 MHz");
    runner.expectEq(uint8_t(2), cpu.loopDelayMs(), "after unthrottle: active loop delay 2ms");
  }

  // === throttle() is idempotent ===
  {
    snapix::drivers::Cpu cpu;
    g_mockCpuFreqMhz = 160;

    cpu.throttle();
    g_mockCpuFreqMhz = 999;  // Sabotage to detect extra call
    cpu.throttle();           // Should be no-op

    runner.expectEq(uint32_t(999), g_mockCpuFreqMhz, "double throttle: no second setCpuFrequencyMhz call");
  }

  // === unthrottle() is idempotent ===
  {
    snapix::drivers::Cpu cpu;
    g_mockCpuFreqMhz = 160;

    cpu.unthrottle();  // Already not throttled — should be no-op

    runner.expectEq(uint32_t(160), g_mockCpuFreqMhz, "unthrottle when not throttled: no setCpuFrequencyMhz call");
  }

  // === throttle -> unthrottle -> throttle cycle ===
  {
    snapix::drivers::Cpu cpu;
    g_mockCpuFreqMhz = 160;

    cpu.throttle();
    runner.expectEq(uint32_t(10), g_mockCpuFreqMhz, "cycle: first throttle sets 10");

    cpu.unthrottle();
    runner.expectEq(uint32_t(160), g_mockCpuFreqMhz, "cycle: unthrottle restores 160");

    cpu.throttle();
    runner.expectEq(uint32_t(10), g_mockCpuFreqMhz, "cycle: second throttle sets 10 again");
  }

  return runner.allPassed() ? 0 : 1;
}
