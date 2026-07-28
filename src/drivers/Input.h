#pragma once

#include <cstdint>

#include "../core/EventQueue.h"
#include "../core/Result.h"
#include "../core/Types.h"
#include "PowerButtonEdgeCapture.h"

class InputManager;
class MappedInputManager;

namespace snapix {
namespace drivers {

class Input {
 public:
  // Threshold for long press detection (ms)
  static constexpr uint32_t LONG_PRESS_MS = 700;

  // Button repeat timing (ms)
  static constexpr uint32_t REPEAT_START_MS = 700;
  static constexpr uint32_t REPEAT_INTERVAL_MS = 350;

  // Only directional buttons repeat (Up=0x01, Down=0x02, Left=0x04, Right=0x08)
  static constexpr uint8_t REPEAT_BUTTON_MASK = 0x0F;

  Result<void> init(EventQueue& eventQueue);
  void shutdown();

  // Call each frame to check buttons and push events
  void poll();

  // Time since last input activity (ms)
  uint32_t idleTimeMs() const;

  // Reset idle timer (e.g., when WiFi activity should prevent auto-sleep)
  void resetIdleTimer();

  // Direct state queries (for hold detection)
  bool isPressed(Button btn) const;

  // Re-read button state after input mapping change to prevent ghost events
  void resyncState();

  // Access underlying input manager (for legacy code during migration)
  MappedInputManager& raw();

 private:
  EventQueue* queue_ = nullptr;
  uint32_t lastActivityMs_ = 0;
  bool initialized_ = false;

  // Track the debounced logical state delivered by the edge captures.
  uint8_t currButtonState_ = 0;

  // Track press start time for long press
  uint32_t pressStartMs_[7] = {};  // One per Button enum value

  // Track repeat timing and long press state
  uint32_t lastRepeatMs_[7] = {};
  bool longPressFired_[7] = {};

  static constexpr uint8_t POWER_BUTTON_MASK = 1 << 6;
  static constexpr size_t POWER_EDGE_CAPACITY = 16;

  PowerButtonEdgeCapture<POWER_EDGE_CAPACITY> powerEdges_;
  bool powerInterruptAttached_ = false;
  bool powerEventDown_ = false;
  uint32_t lastReportedPowerEdgeDrops_ = 0;
  uint32_t lastReportedAdcEdgeDrops_ = 0;

  void pollAdcButtons();
  void pollHeldButtons();
  void processAdcButtonEdge(Button btn, bool pressed, uint32_t timestampMs);
  bool logicalButtonForPhysical(uint8_t physicalButton, Button& logicalButton) const;
  void reconcileAdcState();
  void pollPowerButton();
  void processPowerEdge(bool pressed, uint32_t timestampMs);
  void resetPowerCapture();
  static void powerButtonIsr(void* arg);
};

}  // namespace drivers
}  // namespace snapix
