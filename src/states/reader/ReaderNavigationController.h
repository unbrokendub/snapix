#pragma once

#include <cstdint>

namespace snapix::reader {

class ReaderNavigationController {
 public:
  void resetSession();

  bool& holdNavigatedRef() { return holdNavigated_; }
  uint32_t& powerPressStartedMsRef() { return powerPressStartedMs_; }

  bool holdNavigated() const { return holdNavigated_; }
  void setHoldNavigated(bool value) { holdNavigated_ = value; }

  uint32_t powerPressStartedMs() const { return powerPressStartedMs_; }
  void setPowerPressStartedMs(uint32_t value) { powerPressStartedMs_ = value; }

 private:
  bool holdNavigated_ = false;
  uint32_t powerPressStartedMs_ = 0;
};

}  // namespace snapix::reader
