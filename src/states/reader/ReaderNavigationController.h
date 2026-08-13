#pragma once

#include <cstdint>

namespace snapix::reader {

class ReaderNavigationController {
 public:
  void resetSession();

  bool& holdNavigatedRef() { return holdNavigated_; }
  bool& powerPressActiveRef() { return powerPressActive_; }
  uint32_t& powerPressStartedMsRef() { return powerPressStartedMs_; }

  bool holdNavigated() const { return holdNavigated_; }
  void setHoldNavigated(bool value) { holdNavigated_ = value; }

  bool powerPressActive() const { return powerPressActive_; }
  void setPowerPressActive(bool value) { powerPressActive_ = value; }

  uint32_t powerPressStartedMs() const { return powerPressStartedMs_; }
  void setPowerPressStartedMs(uint32_t value) { powerPressStartedMs_ = value; }

  // Press-seen guard — the same rule `powerPressActive_` already enforces for
  // Power, generalized to every button: a release whose press was consumed by
  // a previous state (opening a book from Home, waking on a button) must not
  // navigate here. Such a release otherwise turns a page during reader entry
  // load, which on a cold book can abort the first section's cache build.
  void notePress(int buttonId) { pressSeenMask_ |= maskBit(buttonId); }
  bool consumeRelease(int buttonId) {
    const uint32_t bit = maskBit(buttonId);
    const bool seen = (pressSeenMask_ & bit) != 0;
    pressSeenMask_ &= ~bit;
    return seen;
  }

 private:
  static uint32_t maskBit(int buttonId) {
    return (buttonId >= 0 && buttonId < 32) ? (1u << buttonId) : 0u;
  }

  bool holdNavigated_ = false;
  bool powerPressActive_ = false;
  uint32_t powerPressStartedMs_ = 0;
  uint32_t pressSeenMask_ = 0;
};

}  // namespace snapix::reader
