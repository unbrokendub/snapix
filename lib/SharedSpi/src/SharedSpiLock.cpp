#include "SharedSpiLock.h"

#include <freertos/task.h>

namespace snapix::spi {

// ── Manual recursive lock backed by a binary semaphore ────────────
//
// FreeRTOS recursive mutexes carry priority inheritance.  Releasing a
// priority-inheriting mutex calls xTaskPriorityDisinherit(), whose
// assertion (pxTCB == pxCurrentTCBs[0]) fires on single-core ESP32-C3
// when the Arduino SPI driver's own mutex interacts with nested
// acquisitions of the shared-bus lock.
//
// A binary semaphore has NO priority inheritance — xSemaphoreGive()
// never invokes xTaskPriorityDisinherit().  We track ownership and
// recursion depth manually under a short critical section, giving the
// same recursive-lock semantics without the problematic assertion.

namespace {
SemaphoreHandle_t gBusSem = nullptr;
portMUX_TYPE gBusMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t gBusOwner = nullptr;
uint32_t gBusDepth = 0;

void ensureInit() {
  if (gBusSem) return;
  portENTER_CRITICAL(&gBusMux);
  if (!gBusSem) {
    gBusSem = xSemaphoreCreateBinary();
    xSemaphoreGive(gBusSem);  // start unlocked
  }
  portEXIT_CRITICAL(&gBusMux);
}

bool busTake(TickType_t timeout) {
  ensureInit();
  TaskHandle_t self = xTaskGetCurrentTaskHandle();

  // Fast path: already owned by this task — bump depth.
  portENTER_CRITICAL(&gBusMux);
  if (gBusOwner == self) {
    ++gBusDepth;
    portEXIT_CRITICAL(&gBusMux);
    return true;
  }
  portEXIT_CRITICAL(&gBusMux);

  // Slow path: block until semaphore is available.
  if (xSemaphoreTake(gBusSem, timeout) != pdTRUE) return false;

  portENTER_CRITICAL(&gBusMux);
  gBusOwner = self;
  gBusDepth = 1;
  portEXIT_CRITICAL(&gBusMux);
  return true;
}

void busGive() {
  portENTER_CRITICAL(&gBusMux);
  if (gBusDepth > 1) {
    --gBusDepth;
    portEXIT_CRITICAL(&gBusMux);
    return;
  }
  // Depth was 1 → fully release.
  gBusOwner = nullptr;
  gBusDepth = 0;
  portEXIT_CRITICAL(&gBusMux);
  xSemaphoreGive(gBusSem);
}

}  // namespace

SemaphoreHandle_t sharedBusMutex() {
  ensureInit();
  return gBusSem;
}

SharedBusLock::SharedBusLock(const TickType_t timeout) : mutex_(nullptr) {
  acquired_ = busTake(timeout);
}

SharedBusLock::~SharedBusLock() {
  if (acquired_) {
    busGive();
  }
}

SharedBusLock::SharedBusLock(SharedBusLock&& other) noexcept : mutex_(nullptr), acquired_(other.acquired_) {
  other.acquired_ = false;
}

}  // namespace snapix::spi
