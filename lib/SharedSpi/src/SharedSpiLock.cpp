#include "SharedSpiLock.h"

namespace papyrix::spi {

namespace {
SemaphoreHandle_t gSharedBusMutex = nullptr;
portMUX_TYPE gSharedBusInitMux = portMUX_INITIALIZER_UNLOCKED;
}  // namespace

SemaphoreHandle_t sharedBusMutex() {
  if (gSharedBusMutex) {
    return gSharedBusMutex;
  }

  portENTER_CRITICAL(&gSharedBusInitMux);
  if (!gSharedBusMutex) {
    // Recursive mutex lets one task guard a high-level SPI pipeline
    // while lower-level helpers safely reacquire the same bus lock.
    gSharedBusMutex = xSemaphoreCreateRecursiveMutex();
  }
  portEXIT_CRITICAL(&gSharedBusInitMux);
  return gSharedBusMutex;
}

SharedBusLock::SharedBusLock(const TickType_t timeout) : mutex_(sharedBusMutex()) {
  if (mutex_) {
    acquired_ = (xSemaphoreTakeRecursive(mutex_, timeout) == pdTRUE);
  }
}

SharedBusLock::~SharedBusLock() {
  if (acquired_ && mutex_) {
    xSemaphoreGiveRecursive(mutex_);
  }
}

SharedBusLock::SharedBusLock(SharedBusLock&& other) noexcept : mutex_(other.mutex_), acquired_(other.acquired_) {
  other.mutex_ = nullptr;
  other.acquired_ = false;
}

}  // namespace papyrix::spi
