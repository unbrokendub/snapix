#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace papyrix::spi {

SemaphoreHandle_t sharedBusMutex();

class SharedBusLock {
 public:
  explicit SharedBusLock(const TickType_t timeout = portMAX_DELAY);
  ~SharedBusLock();

  SharedBusLock(const SharedBusLock&) = delete;
  SharedBusLock& operator=(const SharedBusLock&) = delete;

  SharedBusLock(SharedBusLock&& other) noexcept;
  SharedBusLock& operator=(SharedBusLock&&) = delete;

  bool acquired() const { return acquired_; }
  explicit operator bool() const { return acquired_; }

 private:
  SemaphoreHandle_t mutex_ = nullptr;
  bool acquired_ = false;
};

}  // namespace papyrix::spi
