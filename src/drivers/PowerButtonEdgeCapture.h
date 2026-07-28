#pragma once

#include <cstddef>
#include <cstdint>

namespace snapix {
namespace drivers {

struct PowerButtonEdge {
  bool pressed = false;
  uint32_t timestampUs = 0;
};

// Single-producer (GPIO ISR), single-consumer (main loop) edge buffer.
//
// The old input path sampled Power only from loop().  A complete click that
// happened while the loop was blocked in flash/SD work was therefore
// invisible.  This buffer records both GPIO edges immediately and lets the
// normal input driver publish them to EventQueue later, outside interrupt
// context.
template <size_t Capacity>
class PowerButtonEdgeCapture {
  static_assert(Capacity >= 2, "Power edge buffer must hold at least one edge");
  static_assert(Capacity <= UINT8_MAX, "Power edge indices must fit in uint8_t");

 public:
  static constexpr uint32_t kDebounceUs = 5000;

  // Call before enabling the GPIO interrupt (or while it is disabled).
  void reset(bool pressed, uint32_t timestampUs) {
    head_ = 0;
    tail_ = 0;
    acceptedPressed_ = pressed;
    // Let the first real transition through even if it happens immediately
    // after reset.
    lastAcceptedTimestampUs_ = timestampUs - kDebounceUs;
    compilerBarrier();
  }

  // Kept inline so an IRAM GPIO handler does not call back into flash.
  __attribute__((always_inline)) bool captureFromIsr(bool pressed, uint32_t timestampUs) {
    const bool accepted = acceptedPressed_;
    if (pressed == accepted) {
      return false;
    }

    const uint32_t lastAccepted = lastAcceptedTimestampUs_;
    if (timestampUs - lastAccepted < kDebounceUs) {
      return false;
    }

    acceptedPressed_ = pressed;
    lastAcceptedTimestampUs_ = timestampUs;

    const uint8_t head = head_;
    const uint8_t next = increment(head);
    if (next == tail_) {
      dropped_ = dropped_ + 1;
      return false;
    }

    edges_[head] = {pressed, timestampUs};
    compilerBarrier();
    head_ = next;
    return true;
  }

  bool pop(PowerButtonEdge& edge) {
    const uint8_t tail = tail_;
    if (tail == head_) {
      return false;
    }

    compilerBarrier();
    edge = edges_[tail];
    compilerBarrier();
    tail_ = increment(tail);
    return true;
  }

  uint32_t lastAcceptedTimestampUs() const { return lastAcceptedTimestampUs_; }

  uint32_t droppedCount() const { return dropped_; }

 private:
  __attribute__((always_inline)) static void compilerBarrier() {
#if defined(__GNUC__)
    __asm__ __volatile__("" ::: "memory");
#endif
  }

  static constexpr uint8_t increment(uint8_t value) {
    return static_cast<uint8_t>((static_cast<size_t>(value) + 1) % Capacity);
  }

  PowerButtonEdge edges_[Capacity] = {};
  // One-byte head/tail loads and stores are indivisible on ESP32-C3. There is
  // one writer per index (ISR owns head, loop owns tail); compiler barriers
  // publish/consume each payload in the required order without calling a
  // non-IRAM atomic runtime helper.
  volatile uint8_t head_ = 0;
  volatile uint8_t tail_ = 0;
  volatile bool acceptedPressed_ = false;
  volatile uint32_t lastAcceptedTimestampUs_ = 0;
  volatile uint32_t dropped_ = 0;
};

}  // namespace drivers
}  // namespace snapix
