#pragma once

#include <Arduino.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>

#include "AdcButtonDebouncer.h"

class InputManager {
 public:
  struct ButtonEdge {
    uint8_t buttonIndex;
    bool pressed;
    uint32_t timestampMs;
  };

  InputManager();
  void begin();
  uint8_t getState();

  /**
   * Updates the button states. Should be called regularly in the main loop.
   */
  void update();

  /**
   * Returns true if the button was being held at the time of the last #update() call.
   *
   * @param buttonIndex the button indexes
   * @return the button current press state
   */
  bool isPressed(uint8_t buttonIndex) const;

  /**
   * Returns true if the button went from unpressed to pressed between the last two #update() calls.
   *
   * This differs from #isPressed() in that pressing and holding a button will cause this function
   * to return true after the first #update() call, but false on subsequent calls, whereas #isPressed()
   * will continue to return true.
   *
   * @param buttonIndex
   * @return the button pressed state
   */
  bool wasPressed(uint8_t buttonIndex) const;

  /**
   * Returns true if any button started being pressed between the last two #update() calls
   *
   * @return true if any button started being pressed between the last two #update() calls
   */
  bool wasAnyPressed() const;

  /**
   * Returns true if the button went from pressed to unpressed between the last two #update() calls
   *
   * @param buttonIndex the button indexes
   * @return the button release state
   */
  bool wasReleased(uint8_t buttonIndex) const;

  /**
   * Returns true if any button was released between the last two #update() calls
   *
   * @return  true if any button was released between the last two #update() calls
   */
  bool wasAnyReleased() const;

  /**
   * Returns the time between any button starting to be depressed and all buttons between released
   *
   * @return duration in milliseconds
   */
  unsigned long getHeldTime() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;

  // Pins
  static constexpr int BUTTON_ADC_PIN_1 = 1;
  static constexpr int BUTTON_ADC_PIN_2 = 2;
  static constexpr int POWER_BUTTON_PIN = 3;

  // Power button methods
  bool isPowerButtonPressed() const;

  // Lossless ADC edge capture consumed by the higher-level input driver.
  bool popButtonEdge(ButtonEdge& edge);
  void clearButtonEdges();
  bool isSampledPressed(uint8_t buttonIndex) const;
  bool buttonEdgeCaptureAvailable() const { return buttonEdgeQueue_ != nullptr; }
  bool buttonSamplerRunning() const { return buttonSamplerRunning_.load(std::memory_order_acquire); }
  uint32_t droppedButtonEdgeCount() const { return droppedButtonEdges_.load(std::memory_order_acquire); }

  // Button names
  static const char* getButtonName(uint8_t buttonIndex);

 private:
  int getButtonFromADC(int adcValue, const int ranges[], int numButtons) const;
  void sampleAdcButtons(uint32_t timestampMs);
  void publishTransition(const AdcButtonDebouncer::Transition& transition, uint8_t buttonOffset);
  void enqueueButtonEdge(uint8_t buttonIndex, bool pressed, uint32_t timestampMs);
  void updateAdcStableState();
  static void buttonSamplerTaskEntry(void* arg);
  void buttonSamplerTask();

  uint8_t currentState;
  uint8_t pressedEvents;
  uint8_t releasedEvents;
  unsigned long buttonPressStart;
  unsigned long buttonPressFinish;

  AdcButtonDebouncer adcLine1_;
  AdcButtonDebouncer adcLine2_;
  std::atomic<uint8_t> adcSampledState_{0};
  std::atomic<uint8_t> adcStableState_{0};
  QueueHandle_t buttonEdgeQueue_ = nullptr;
  TaskHandle_t buttonSamplerTask_ = nullptr;
  std::atomic<bool> buttonSamplerRunning_{false};
  std::atomic<uint32_t> droppedButtonEdges_{0};
  bool begun_ = false;

  bool powerCandidatePressed_ = false;
  bool powerStablePressed_ = false;
  uint32_t powerCandidateSinceMs_ = 0;

  static constexpr int NUM_BUTTONS_1 = 4;
  static const int ADC_RANGES_1[];

  static constexpr int NUM_BUTTONS_2 = 2;
  static const int ADC_RANGES_2[];

  static constexpr int ADC_NO_BUTTON = 3800;
  static constexpr unsigned long DEBOUNCE_DELAY = 5;
  static constexpr UBaseType_t BUTTON_EDGE_QUEUE_CAPACITY = 32;
  static constexpr uint32_t BUTTON_SAMPLE_INTERVAL_MS = 8;
  static constexpr uint32_t BUTTON_SAMPLER_STACK_BYTES = 3072;
  static constexpr UBaseType_t BUTTON_SAMPLER_PRIORITY = 2;

  static const char* BUTTON_NAMES[];
};

// Disable internal pull-ups/pull-downs on all GPIOs to minimize leakage current during deep sleep.
// Skips POWER_BUTTON_PIN (wakeup source — needs pull-up to avoid floating/spurious wakeups).
inline void disableGpioPullsForSleep() {
  static constexpr gpio_num_t pins[] = {
      GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2,  GPIO_NUM_4,  GPIO_NUM_5,  GPIO_NUM_6,  GPIO_NUM_7,
      GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_13, GPIO_NUM_20, GPIO_NUM_21,
  };
  for (auto pin : pins) {
    gpio_pullup_dis(pin);
    gpio_pulldown_dis(pin);
  }
}
