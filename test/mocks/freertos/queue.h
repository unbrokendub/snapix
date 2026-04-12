#pragma once

#include "FreeRTOS.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

struct MockQueue {
  std::mutex mutex;
  std::condition_variable cv;
  size_t itemSize = 0;
  size_t capacity = 0;
  std::deque<std::vector<uint8_t>> items;
};

inline std::vector<MockQueue*>& getQueueRegistry() {
  static std::vector<MockQueue*> registry;
  return registry;
}

inline QueueHandle_t xQueueCreate(const UBaseType_t uxQueueLength, const UBaseType_t uxItemSize) {
  MockQueue* queue = new MockQueue();
  queue->capacity = uxQueueLength;
  queue->itemSize = uxItemSize;
  getQueueRegistry().push_back(queue);
  return static_cast<QueueHandle_t>(queue);
}

inline void vQueueDelete(QueueHandle_t xQueue) {
  MockQueue* queue = static_cast<MockQueue*>(xQueue);
  auto& registry = getQueueRegistry();
  registry.erase(std::remove(registry.begin(), registry.end(), queue), registry.end());
  delete queue;
}

inline BaseType_t xQueueSend(QueueHandle_t xQueue, const void* pvItemToQueue, TickType_t xTicksToWait) {
  MockQueue* queue = static_cast<MockQueue*>(xQueue);
  if (!queue || !pvItemToQueue) {
    return pdFAIL;
  }

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(xTicksToWait);
  std::unique_lock<std::mutex> lock(queue->mutex);
  while (queue->items.size() >= queue->capacity) {
    if (xTicksToWait == 0) {
      return pdFAIL;
    }
    if (queue->cv.wait_until(lock, deadline) == std::cv_status::timeout) {
      return pdFAIL;
    }
  }

  const uint8_t* bytes = static_cast<const uint8_t*>(pvItemToQueue);
  queue->items.emplace_back(bytes, bytes + queue->itemSize);
  lock.unlock();
  queue->cv.notify_all();
  return pdTRUE;
}

inline BaseType_t xQueueReceive(QueueHandle_t xQueue, void* pvBuffer, TickType_t xTicksToWait) {
  MockQueue* queue = static_cast<MockQueue*>(xQueue);
  if (!queue || !pvBuffer) {
    return pdFAIL;
  }

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(xTicksToWait);
  std::unique_lock<std::mutex> lock(queue->mutex);
  while (queue->items.empty()) {
    if (xTicksToWait == 0) {
      return pdFAIL;
    }
    if (queue->cv.wait_until(lock, deadline) == std::cv_status::timeout) {
      return pdFAIL;
    }
  }

  const std::vector<uint8_t>& item = queue->items.front();
  memcpy(pvBuffer, item.data(), queue->itemSize);
  queue->items.pop_front();
  lock.unlock();
  queue->cv.notify_all();
  return pdTRUE;
}

inline void cleanupMockQueues() {
  auto& registry = getQueueRegistry();
  for (auto* queue : registry) {
    delete queue;
  }
  registry.clear();
}
