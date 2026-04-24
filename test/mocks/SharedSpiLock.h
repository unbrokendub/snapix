#pragma once

namespace snapix::spi {

class SharedBusLock {
 public:
  SharedBusLock() = default;
  explicit operator bool() const { return true; }
};

}  // namespace snapix::spi
