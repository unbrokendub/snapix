#include "Input.h"

#include <Arduino.h>
#include <InputManager.h>
#include <Logging.h>
#include <MappedInputManager.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <hal/gpio_ll.h>

#include <algorithm>

#define TAG "INPUT"

// Global input managers (defined in main.cpp)
extern InputManager inputManager;
extern MappedInputManager& mappedInput;

namespace snapix {
namespace drivers {

Result<void> Input::init(EventQueue& eventQueue) {
  if (initialized_) {
    return Ok();
  }

  queue_ = &eventQueue;
  lastActivityMs_ = millis();
  currButtonState_ = 0;
  inputManager.clearButtonEdges();
  lastReportedAdcEdgeDrops_ = inputManager.droppedButtonEdgeCount();
  resetPowerCapture();

  // Install the GPIO service with IRAM support before registering the handler.
  // This keeps Power edge capture alive while flash/LittleFS temporarily makes
  // normal code unavailable. No other project code installs this service.
  const esp_err_t installResult = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (installResult == ESP_OK || installResult == ESP_ERR_INVALID_STATE) {
    const esp_err_t typeResult =
        gpio_set_intr_type(static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN), GPIO_INTR_ANYEDGE);
    const esp_err_t addResult =
        gpio_isr_handler_add(static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN), &Input::powerButtonIsr, this);
    powerInterruptAttached_ = typeResult == ESP_OK && addResult == ESP_OK;
    if (!powerInterruptAttached_) {
      if (addResult == ESP_OK) {
        gpio_isr_handler_remove(static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN));
      }
      LOG_ERR(TAG, "Power GPIO interrupt unavailable type=%d add=%d; using polling fallback",
              static_cast<int>(typeResult), static_cast<int>(addResult));
    }
  } else {
    LOG_ERR(TAG, "Power GPIO ISR service unavailable err=%d; using polling fallback",
            static_cast<int>(installResult));
  }

  initialized_ = true;
  if (inputManager.buttonSamplerRunning()) {
    LOG_INF(TAG, "Independent ADC button sampler active");
  } else {
    LOG_ERR(TAG, "Independent ADC button sampler unavailable; using loop polling fallback");
  }

  return Ok();
}

void Input::shutdown() {
  if (powerInterruptAttached_) {
    gpio_isr_handler_remove(static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN));
    powerInterruptAttached_ = false;
  }
  queue_ = nullptr;
  initialized_ = false;
}

void Input::poll() {
  if (!initialized_ || !queue_) {
    return;
  }

  // Drain all physical ADC transitions before considering holds. In
  // particular, a release captured during a long render must clear the held
  // state before repeat generation, otherwise a short click can become a
  // false chapter jump and its later release can look "eaten".
  pollAdcButtons();
  pollPowerButton();
  pollHeldButtons();
}

void Input::pollAdcButtons() {
  if (!inputManager.buttonEdgeCaptureAvailable()) {
    // Queue allocation failure is exceptionally rare, but keep every button
    // usable through the old level-polled behavior.
    constexpr Button buttons[] = {Button::Up, Button::Down, Button::Left,
                                  Button::Right, Button::Center, Button::Back};
    for (const Button button : buttons) {
      const uint8_t mask = 1U << static_cast<uint8_t>(button);
      const bool pressed = isPressed(button);
      if (pressed != ((currButtonState_ & mask) != 0)) {
        processAdcButtonEdge(button, pressed, millis());
      }
    }
    return;
  }

  InputManager::ButtonEdge edge;
  while (inputManager.popButtonEdge(edge)) {
    Button logicalButton;
    if (logicalButtonForPhysical(edge.buttonIndex, logicalButton)) {
      processAdcButtonEdge(logicalButton, edge.pressed, edge.timestampMs);
    }
  }

  const uint32_t dropped = inputManager.droppedButtonEdgeCount();
  if (dropped != lastReportedAdcEdgeDrops_) {
    LOG_ERR(TAG, "ADC edge capture overflow dropped=%lu; reconciling levels",
            static_cast<unsigned long>(dropped));
    lastReportedAdcEdgeDrops_ = dropped;
    reconcileAdcState();
  }
}

void Input::pollHeldButtons() {
  const uint32_t now = millis();
  static constexpr MappedInputManager::Button mappedButtons[] = {
      MappedInputManager::Button::Up,      MappedInputManager::Button::Down,
      MappedInputManager::Button::Left,    MappedInputManager::Button::Right,
      MappedInputManager::Button::Confirm, MappedInputManager::Button::Back,
  };
  for (uint8_t idx = 0; idx < static_cast<uint8_t>(Button::Power); ++idx) {
    const uint8_t mask = 1U << idx;
    if ((currButtonState_ & mask) == 0) {
      continue;
    }

    // The debounced release edge may still be a few milliseconds away. Once
    // the sampler has already observed the line released, never synthesize a
    // Repeat/LongPress from the stale stable-down state.
    if (!mappedInput.isSampledPressed(mappedButtons[idx])) {
      continue;
    }

    const Button button = static_cast<Button>(idx);
    const uint32_t heldMs = now - pressStartMs_[idx];
    if ((mask & REPEAT_BUTTON_MASK) != 0) {
      const uint32_t sinceLastRepeat = now - lastRepeatMs_[idx];
      const uint32_t threshold =
          (lastRepeatMs_[idx] == pressStartMs_[idx]) ? REPEAT_START_MS : REPEAT_INTERVAL_MS;
      if (sinceLastRepeat >= threshold) {
        queue_->push(Event::buttonRepeat(button, now));
        lastRepeatMs_[idx] = now;
        lastActivityMs_ = now;
      }
    } else if (!longPressFired_[idx] && heldMs >= LONG_PRESS_MS) {
      queue_->push(Event::buttonLongPress(button, now));
      longPressFired_[idx] = true;
    }
  }
}

void Input::processAdcButtonEdge(const Button btn, const bool pressed,
                                 const uint32_t timestampMs) {
  const uint8_t idx = static_cast<uint8_t>(btn);
  const uint8_t mask = 1U << idx;
  const bool wasPressed = (currButtonState_ & mask) != 0;
  if (pressed == wasPressed) {
    return;
  }

  const uint32_t deliveredAtMs = millis();
  lastActivityMs_ = deliveredAtMs;
  LOG_DBG(TAG, "ADC edge button=%u pressed=%u edgeMs=%lu deliveryDelay=%lu",
          static_cast<unsigned>(idx), static_cast<unsigned>(pressed),
          static_cast<unsigned long>(timestampMs),
          static_cast<unsigned long>(deliveredAtMs - timestampMs));

  if (pressed) {
    currButtonState_ |= mask;
    pressStartMs_[idx] = timestampMs;
    lastRepeatMs_[idx] = timestampMs;
    longPressFired_[idx] = false;
    queue_->push(Event::buttonPress(btn, timestampMs));
  } else {
    currButtonState_ &= static_cast<uint8_t>(~mask);
    queue_->push(Event::buttonRelease(btn, timestampMs));
  }
}

bool Input::logicalButtonForPhysical(const uint8_t physicalButton,
                                     Button& logicalButton) const {
  struct Mapping {
    Button logical;
    MappedInputManager::Button mapped;
  };
  static constexpr Mapping mappings[] = {
      {Button::Up, MappedInputManager::Button::Up},
      {Button::Down, MappedInputManager::Button::Down},
      {Button::Left, MappedInputManager::Button::Left},
      {Button::Right, MappedInputManager::Button::Right},
      {Button::Center, MappedInputManager::Button::Confirm},
      {Button::Back, MappedInputManager::Button::Back},
  };

  const auto* end = mappings + (sizeof(mappings) / sizeof(mappings[0]));
  const auto* match = std::find_if(
      mappings, end, [physicalButton](const Mapping& mapping) {
        return mappedInput.physicalButton(mapping.mapped) == physicalButton;
      });
  if (match != end) {
    logicalButton = match->logical;
    return true;
  }
  return false;
}

void Input::reconcileAdcState() {
  constexpr Button buttons[] = {Button::Up, Button::Down, Button::Left,
                                Button::Right, Button::Center, Button::Back};
  const uint32_t now = millis();
  for (const Button button : buttons) {
    const uint8_t mask = 1U << static_cast<uint8_t>(button);
    const bool sampledPressed = isPressed(button);
    if (sampledPressed != ((currButtonState_ & mask) != 0)) {
      processAdcButtonEdge(button, sampledPressed, now);
    }
  }
}

void Input::pollPowerButton() {
  PowerButtonEdge edge;
  while (powerEdges_.pop(edge)) {
    // Convert the ISR's wrap-safe microsecond timestamp into the millis()
    // domain used by all existing input duration logic.
    const uint32_t nowUs = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t timestampMs = millis() - ((nowUs - edge.timestampUs) / 1000U);
    processPowerEdge(edge.pressed, timestampMs);
  }

  if (powerInterruptAttached_) {
    const uint32_t nowUs = static_cast<uint32_t>(esp_timer_get_time());
    const bool rawPressed =
        gpio_ll_get_level(GPIO_LL_GET_HW(0), InputManager::POWER_BUTTON_PIN) == 0;

    // Defense-in-depth for an edge masked by a critical section. Give the
    // interrupt debounce window time to settle before reconciling raw state.
    if (rawPressed != powerEventDown_ &&
        nowUs - powerEdges_.lastAcceptedTimestampUs() >= PowerButtonEdgeCapture<POWER_EDGE_CAPACITY>::kDebounceUs) {
      gpio_intr_disable(static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN));
      const uint32_t syncedUs = static_cast<uint32_t>(esp_timer_get_time());
      const bool syncedPressed =
          gpio_ll_get_level(GPIO_LL_GET_HW(0), InputManager::POWER_BUTTON_PIN) == 0;
      powerEdges_.reset(syncedPressed, syncedUs);
      gpio_intr_enable(static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN));
      if (syncedPressed != powerEventDown_) {
        LOG_DBG(TAG, "Power edge recovered by GPIO level fallback pressed=%u",
                static_cast<unsigned>(syncedPressed));
        processPowerEdge(syncedPressed, millis());
      }
    }
  } else {
    // Preserve the original debounced polling path if GPIO ISR setup failed.
    const bool polledPressed = mappedInput.isPressed(MappedInputManager::Button::Power);
    if (polledPressed != powerEventDown_) {
      processPowerEdge(polledPressed, millis());
    }
  }

  if (powerEventDown_) {
    currButtonState_ |= POWER_BUTTON_MASK;
    const int idx = static_cast<int>(Button::Power);
    const uint32_t now = millis();
    if (!longPressFired_[idx] && now - pressStartMs_[idx] >= LONG_PRESS_MS) {
      queue_->push(Event::buttonLongPress(Button::Power, now));
      longPressFired_[idx] = true;
    }
  }

  const uint32_t dropped = powerEdges_.droppedCount();
  if (dropped != lastReportedPowerEdgeDrops_) {
    LOG_ERR(TAG, "Power edge capture overflow dropped=%lu", static_cast<unsigned long>(dropped));
    lastReportedPowerEdgeDrops_ = dropped;
  }
}

void Input::processPowerEdge(bool pressed, uint32_t timestampMs) {
  if (pressed == powerEventDown_) {
    return;
  }

  const int idx = static_cast<int>(Button::Power);
  powerEventDown_ = pressed;
  // The Event keeps the physical edge time for press-duration decisions, but
  // power management must wake the CPU based on when activity is delivered.
  const uint32_t deliveredAtMs = millis();
  lastActivityMs_ = deliveredAtMs;
  LOG_DBG(TAG, "Power edge pressed=%u edgeMs=%lu deliveryDelay=%lu",
          static_cast<unsigned>(pressed), static_cast<unsigned long>(timestampMs),
          static_cast<unsigned long>(deliveredAtMs - timestampMs));

  if (pressed) {
    pressStartMs_[idx] = timestampMs;
    lastRepeatMs_[idx] = timestampMs;
    longPressFired_[idx] = false;
    queue_->push(Event::buttonPress(Button::Power, timestampMs));
    currButtonState_ |= POWER_BUTTON_MASK;
  } else {
    queue_->push(Event::buttonRelease(Button::Power, timestampMs));
    currButtonState_ &= static_cast<uint8_t>(~POWER_BUTTON_MASK);
  }
}

void IRAM_ATTR Input::powerButtonIsr(void* arg) {
  auto* input = static_cast<Input*>(arg);
  const bool pressed =
      gpio_ll_get_level(GPIO_LL_GET_HW(0), InputManager::POWER_BUTTON_PIN) == 0;
  input->powerEdges_.captureFromIsr(pressed, static_cast<uint32_t>(esp_timer_get_time()));
}

void Input::resetPowerCapture() {
  const uint32_t nowUs = static_cast<uint32_t>(esp_timer_get_time());
  const bool pressed =
      gpio_ll_get_level(GPIO_LL_GET_HW(0), InputManager::POWER_BUTTON_PIN) == 0;
  powerEdges_.reset(pressed, nowUs);
  powerEventDown_ = pressed;
  lastReportedPowerEdgeDrops_ = powerEdges_.droppedCount();

  const int idx = static_cast<int>(Button::Power);
  pressStartMs_[idx] = millis();
  lastRepeatMs_[idx] = pressStartMs_[idx];
  longPressFired_[idx] = false;
}

uint32_t Input::idleTimeMs() const { return millis() - lastActivityMs_; }

void Input::resetIdleTimer() { lastActivityMs_ = millis(); }

bool Input::isPressed(Button btn) const {
  MappedInputManager::Button mappedBtn;
  switch (btn) {
    case Button::Up:
      mappedBtn = MappedInputManager::Button::Up;
      break;
    case Button::Down:
      mappedBtn = MappedInputManager::Button::Down;
      break;
    case Button::Left:
      mappedBtn = MappedInputManager::Button::Left;
      break;
    case Button::Right:
      mappedBtn = MappedInputManager::Button::Right;
      break;
    case Button::Center:
      mappedBtn = MappedInputManager::Button::Confirm;
      break;
    case Button::Back:
      mappedBtn = MappedInputManager::Button::Back;
      break;
    case Button::Power:
      mappedBtn = MappedInputManager::Button::Power;
      break;
  }
  return mappedInput.isPressed(mappedBtn);
}

void Input::resyncState() {
  if (powerInterruptAttached_) {
    gpio_intr_disable(static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN));
  }
  resetPowerCapture();
  if (powerInterruptAttached_) {
    gpio_intr_enable(static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN));
  }

  inputManager.clearButtonEdges();
  currButtonState_ = 0;
  if (isPressed(Button::Up)) currButtonState_ |= (1 << 0);
  if (isPressed(Button::Down)) currButtonState_ |= (1 << 1);
  if (isPressed(Button::Left)) currButtonState_ |= (1 << 2);
  if (isPressed(Button::Right)) currButtonState_ |= (1 << 3);
  if (isPressed(Button::Center)) currButtonState_ |= (1 << 4);
  if (isPressed(Button::Back)) currButtonState_ |= (1 << 5);
  if (powerEventDown_) currButtonState_ |= POWER_BUTTON_MASK;
  lastReportedAdcEdgeDrops_ = inputManager.droppedButtonEdgeCount();

  // A held button at a mapping/state resync starts a fresh hold interval.
  // Reusing an old timer here could create an immediate ghost Repeat.
  const uint32_t now = millis();
  for (uint8_t idx = 0; idx < 7; ++idx) {
    if ((currButtonState_ & (1U << idx)) != 0) {
      pressStartMs_[idx] = now;
      lastRepeatMs_[idx] = now;
      longPressFired_[idx] = false;
    }
  }
}

MappedInputManager& Input::raw() { return mappedInput; }

}  // namespace drivers
}  // namespace snapix
