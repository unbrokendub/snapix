#pragma once

namespace papyrix::spi {

class SharedBusLock {
 public:
  SharedBusLock() = default;
  explicit operator bool() const { return true; }
};

}  // namespace papyrix::spi
