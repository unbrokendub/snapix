#include "InputManager.h"

// Recorded ADC values from real devices
// BACK CONF LEFT RGHT   UP DOWN
// 3597 2760 1530    6 2300    6
// 3470 2666 1480    6 2222    5
// 3470 2655 1470    3 2205    3

// Averages
// BACK CONF LEFT RGHT   UP DOWN
// 3512 2694 1493    5 2242    5

// Setup ranges, if ADC value is between value `i` and `i + 1`, button `i` is being pressed
// These ranges are based on real world values above, and are much more tolerant of different
// devices than a fixed threshold check
// These values are calculated by taking the midpoint of the pairs of averaged values above
const int InputManager::ADC_RANGES_1[] = {ADC_NO_BUTTON, 3100, 2090, 750, INT32_MIN};
const int InputManager::ADC_RANGES_2[] = {ADC_NO_BUTTON, 1120, INT32_MIN};
const char* InputManager::BUTTON_NAMES[] = {"Back", "Confirm", "Left", "Right", "Up", "Down", "Power"};

InputManager::InputManager()
    : currentState(0),
      pressedEvents(0),
      releasedEvents(0),
      buttonPressStart(0),
      buttonPressFinish(0) {}

void InputManager::begin() {
  if (begun_) {
    return;
  }
  begun_ = true;

  pinMode(BUTTON_ADC_PIN_1, INPUT);
  pinMode(BUTTON_ADC_PIN_2, INPUT);
  pinMode(POWER_BUTTON_PIN, INPUT_PULLUP);
  analogSetAttenuation(ADC_11db);

  const uint32_t now = millis();
  const int button1 = getButtonFromADC(analogRead(BUTTON_ADC_PIN_1), ADC_RANGES_1, NUM_BUTTONS_1);
  const int button2 = getButtonFromADC(analogRead(BUTTON_ADC_PIN_2), ADC_RANGES_2, NUM_BUTTONS_2);
  adcLine1_.reset(static_cast<int8_t>(button1), now);
  adcLine2_.reset(static_cast<int8_t>(button2), now);
  updateAdcStableState();
  adcSampledState_.store(adcStableState_.load(std::memory_order_relaxed),
                         std::memory_order_release);

  powerCandidatePressed_ = digitalRead(POWER_BUTTON_PIN) == LOW;
  powerStablePressed_ = powerCandidatePressed_;
  powerCandidateSinceMs_ = now;

  buttonEdgeQueue_ = xQueueCreate(BUTTON_EDGE_QUEUE_CAPACITY, sizeof(ButtonEdge));
  if (buttonEdgeQueue_ != nullptr) {
    const BaseType_t taskResult =
        xTaskCreate(&InputManager::buttonSamplerTaskEntry, "InputADC", BUTTON_SAMPLER_STACK_BYTES, this,
                    BUTTON_SAMPLER_PRIORITY, &buttonSamplerTask_);
    if (taskResult == pdPASS && buttonSamplerTask_ != nullptr) {
      buttonSamplerRunning_.store(true, std::memory_order_release);
    } else {
      buttonSamplerTask_ = nullptr;
    }
  }
}

int InputManager::getButtonFromADC(const int adcValue, const int ranges[], const int numButtons) const {
  for (int i = 0; i < numButtons; i++) {
    if (ranges[i + 1] < adcValue && adcValue <= ranges[i]) {
      return i;
    }
  }

  return -1;
}

uint8_t InputManager::getState() {
  uint8_t state = adcStableState_.load(std::memory_order_acquire);
  if (powerStablePressed_) {
    state |= (1 << BTN_POWER);
  }
  return state;
}

void InputManager::update() {
  const unsigned long currentTime = millis();

  // Always clear events first
  pressedEvents = 0;
  releasedEvents = 0;

  // If the dedicated task could not be created, retain functional (although
  // not stall-proof) sampling from the caller's loop.
  if (!buttonSamplerRunning_.load(std::memory_order_acquire)) {
    sampleAdcButtons(currentTime);
  }

  // Power has its own independent debounce. ADC noise can no longer reset it,
  // and Power noise can no longer reset either resistor ladder.
  const bool rawPowerPressed = digitalRead(POWER_BUTTON_PIN) == LOW;
  if (rawPowerPressed != powerCandidatePressed_) {
    powerCandidatePressed_ = rawPowerPressed;
    powerCandidateSinceMs_ = currentTime;
  } else if (powerStablePressed_ != powerCandidatePressed_ &&
             currentTime - powerCandidateSinceMs_ >= DEBOUNCE_DELAY) {
    powerStablePressed_ = powerCandidatePressed_;
  }

  const uint8_t state = getState();
  if (state != currentState) {
    pressedEvents = state & ~currentState;
    releasedEvents = currentState & ~state;

    if (pressedEvents > 0 && currentState == 0) {
      buttonPressStart = currentTime;
    }

    if (releasedEvents > 0 && state == 0) {
      buttonPressFinish = currentTime;
    }

    currentState = state;
  }
}

bool InputManager::isPressed(const uint8_t buttonIndex) const {
  if (buttonIndex < BTN_POWER) {
    return (adcStableState_.load(std::memory_order_acquire) & (1 << buttonIndex)) != 0;
  }
  return buttonIndex == BTN_POWER && powerStablePressed_;
}

bool InputManager::wasPressed(const uint8_t buttonIndex) const { return pressedEvents & (1 << buttonIndex); }

bool InputManager::wasAnyPressed() const { return pressedEvents > 0; }

bool InputManager::wasReleased(const uint8_t buttonIndex) const { return releasedEvents & (1 << buttonIndex); }

bool InputManager::wasAnyReleased() const { return releasedEvents > 0; }

unsigned long InputManager::getHeldTime() const {
  // Still hold a button
  if (currentState > 0) {
    return millis() - buttonPressStart;
  }

  return buttonPressFinish - buttonPressStart;
}

const char* InputManager::getButtonName(const uint8_t buttonIndex) {
  if (buttonIndex <= BTN_POWER) {
    return BUTTON_NAMES[buttonIndex];
  }
  return "Unknown";
}

bool InputManager::isPowerButtonPressed() const { return isPressed(BTN_POWER); }

bool InputManager::popButtonEdge(ButtonEdge& edge) {
  return buttonEdgeQueue_ != nullptr && xQueueReceive(buttonEdgeQueue_, &edge, 0) == pdTRUE;
}

void InputManager::clearButtonEdges() {
  if (buttonEdgeQueue_ != nullptr) {
    xQueueReset(buttonEdgeQueue_);
  }
}

bool InputManager::isSampledPressed(const uint8_t buttonIndex) const {
  return buttonIndex < BTN_POWER &&
         (adcSampledState_.load(std::memory_order_acquire) & (1U << buttonIndex)) != 0;
}

void InputManager::sampleAdcButtons(const uint32_t timestampMs) {
  const int button1 = getButtonFromADC(analogRead(BUTTON_ADC_PIN_1), ADC_RANGES_1, NUM_BUTTONS_1);
  const int button2 = getButtonFromADC(analogRead(BUTTON_ADC_PIN_2), ADC_RANGES_2, NUM_BUTTONS_2);

  uint8_t sampledState = 0;
  if (button1 >= 0) {
    sampledState |= 1U << static_cast<uint8_t>(button1);
  }
  if (button2 >= 0) {
    sampledState |= 1U << (static_cast<uint8_t>(button2) + 4U);
  }
  adcSampledState_.store(sampledState, std::memory_order_release);

  const auto transition1 = adcLine1_.observe(static_cast<int8_t>(button1), timestampMs);
  if (transition1.changed) {
    publishTransition(transition1, 0);
  }

  const auto transition2 = adcLine2_.observe(static_cast<int8_t>(button2), timestampMs);
  if (transition2.changed) {
    publishTransition(transition2, 4);
  }
}

void InputManager::publishTransition(const AdcButtonDebouncer::Transition& transition,
                                     const uint8_t buttonOffset) {
  // Make direct state queries current before publishing the corresponding
  // ordered edges. Edge consumers normally trust the queue; its drop counter
  // tells them when a level reconciliation is needed.
  updateAdcStableState();

  if (transition.previousButton >= 0) {
    enqueueButtonEdge(static_cast<uint8_t>(transition.previousButton) + buttonOffset, false,
                      transition.timestampMs);
  }
  if (transition.currentButton >= 0) {
    enqueueButtonEdge(static_cast<uint8_t>(transition.currentButton) + buttonOffset, true,
                      transition.timestampMs);
  }
}

void InputManager::enqueueButtonEdge(const uint8_t buttonIndex, const bool pressed,
                                     const uint32_t timestampMs) {
  if (buttonEdgeQueue_ == nullptr) {
    return;
  }

  const ButtonEdge edge{buttonIndex, pressed, timestampMs};
  if (xQueueSend(buttonEdgeQueue_, &edge, 0) != pdTRUE) {
    droppedButtonEdges_.fetch_add(1, std::memory_order_relaxed);
  }
}

void InputManager::updateAdcStableState() {
  uint8_t state = 0;
  if (adcLine1_.stableButton() >= 0) {
    state |= 1U << static_cast<uint8_t>(adcLine1_.stableButton());
  }
  if (adcLine2_.stableButton() >= 0) {
    state |= 1U << (static_cast<uint8_t>(adcLine2_.stableButton()) + 4U);
  }
  adcStableState_.store(state, std::memory_order_release);
}

void InputManager::buttonSamplerTaskEntry(void* arg) {
  static_cast<InputManager*>(arg)->buttonSamplerTask();
}

void InputManager::buttonSamplerTask() {
  while (true) {
    sampleAdcButtons(millis());
    // A relative delay intentionally avoids a burst of "catch-up" ADC reads
    // after flash/cache activity paused this task for longer than one period.
    vTaskDelay(pdMS_TO_TICKS(BUTTON_SAMPLE_INTERVAL_MS));
  }
}
