#pragma once

#include <cstdint>

namespace snapix {
namespace drivers {

class Cpu {
 public:
  void throttle();
  void unthrottle();
  bool isThrottled() const;
  uint8_t loopDelayMs() const;

 private:
  bool throttled_ = false;
};

}  // namespace drivers
}  // namespace snapix
