#pragma once

#include <cstdint>

/**
 * Debounces one resistor-ladder input independently from every other ladder.
 *
 * The caller supplies the decoded button number (-1 means no button).  A
 * transition is published only after the same candidate has remained stable
 * for kDebounceMs.  timestampMs records the first observation of that stable
 * candidate, not the later delivery time.
 */
class AdcButtonDebouncer {
 public:
  static constexpr uint32_t kDebounceMs = 5;

  struct Transition {
    int8_t previousButton = -1;
    int8_t currentButton = -1;
    uint32_t timestampMs = 0;
    bool changed = false;
  };

  void reset(int8_t button, uint32_t timestampMs) {
    stableButton_ = button;
    candidateButton_ = button;
    candidateSinceMs_ = timestampMs;
  }

  Transition observe(int8_t button, uint32_t timestampMs) {
    if (button != candidateButton_) {
      candidateButton_ = button;
      candidateSinceMs_ = timestampMs;
      return {};
    }

    if (candidateButton_ == stableButton_ ||
        timestampMs - candidateSinceMs_ < kDebounceMs) {
      return {};
    }

    const int8_t previous = stableButton_;
    stableButton_ = candidateButton_;
    return {previous, stableButton_, candidateSinceMs_, true};
  }

  int8_t stableButton() const { return stableButton_; }

 private:
  int8_t stableButton_ = -1;
  int8_t candidateButton_ = -1;
  uint32_t candidateSinceMs_ = 0;
};
