#include "Cpu.h"

#include <Arduino.h>

namespace papyrix {
namespace drivers {

static constexpr uint8_t kIdleFreqMhz = 10;
static constexpr uint8_t kActiveFreqMhz = 160;
// Более короткий sleep в основном цикле заметно снижает input/UI latency.
// Значения оставлены достаточно консервативными, чтобы не устроить бессмысленный busy-spin.
static constexpr uint8_t kIdleLoopDelayMs = 20;
static constexpr uint8_t kActiveLoopDelayMs = 2;

void Cpu::throttle() {
  if (!throttled_) {
    setCpuFrequencyMhz(kIdleFreqMhz);
    throttled_ = true;
  }
}

void Cpu::unthrottle() {
  if (throttled_) {
    setCpuFrequencyMhz(kActiveFreqMhz);
    throttled_ = false;
  }
}

bool Cpu::isThrottled() const { return throttled_; }

uint8_t Cpu::loopDelayMs() const { return throttled_ ? kIdleLoopDelayMs : kActiveLoopDelayMs; }

}  // namespace drivers
}  // namespace papyrix
