// FreeRTOS stand-ins on std::thread / std::mutex: tasks, mutexes, critical
// sections and stream buffers - what Obd.h, History.h and the sketch use.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

typedef uint32_t TickType_t;
typedef int BaseType_t;
#define portMAX_DELAY 0xFFFFFFFFu
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

inline void vTaskDelay(TickType_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

typedef void *TaskHandle_t;
inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *, uint32_t, void *arg, int, TaskHandle_t *,
                                          int) {
  std::thread(fn, arg).detach();
  return pdPASS;
}
inline BaseType_t xTaskCreate(void (*fn)(void *), const char *n, uint32_t s, void *a, int p, TaskHandle_t *h) {
  return xTaskCreatePinnedToCore(fn, n, s, a, p, h, 0);
}

// ---- mutex semaphores
typedef std::timed_mutex *SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::timed_mutex(); }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t ms) {
  if (ms == portMAX_DELAY) { m->lock(); return pdTRUE; }
  return m->try_lock_for(std::chrono::milliseconds(ms)) ? pdTRUE : pdFALSE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { m->unlock(); return pdTRUE; }

// ---- critical sections (spinlocks on the ESP32)
struct portMUX_TYPE {
  std::recursive_mutex m;
};
#define portMUX_INITIALIZER_UNLOCKED {}
inline void portENTER_CRITICAL(portMUX_TYPE *x) { x->m.lock(); }
inline void portEXIT_CRITICAL(portMUX_TYPE *x) { x->m.unlock(); }
inline void portENTER_CRITICAL_ISR(portMUX_TYPE *x) { x->m.lock(); }
inline void portEXIT_CRITICAL_ISR(portMUX_TYPE *x) { x->m.unlock(); }

// ---- stream buffers (byte FIFO with a blocking receive)
struct StreamBufferDef {
  std::mutex m;
  std::condition_variable cv;
  std::deque<uint8_t> q;
  size_t cap;
};
typedef StreamBufferDef *StreamBufferHandle_t;
inline StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t) {
  auto *b = new StreamBufferDef();
  b->cap = size;
  return b;
}
inline size_t xStreamBufferSend(StreamBufferHandle_t b, const void *data, size_t n, TickType_t) {
  std::lock_guard<std::mutex> l(b->m);
  size_t k = std::min(n, b->cap - b->q.size());
  const uint8_t *p = (const uint8_t *)data;
  b->q.insert(b->q.end(), p, p + k);
  b->cv.notify_all();
  return k;
}
inline size_t xStreamBufferReceive(StreamBufferHandle_t b, void *out, size_t n, TickType_t ms) {
  std::unique_lock<std::mutex> l(b->m);
  if (b->q.empty() && ms) b->cv.wait_for(l, std::chrono::milliseconds(ms == portMAX_DELAY ? 3600000 : ms));
  size_t k = std::min(n, b->q.size());
  for (size_t i = 0; i < k; i++) {
    ((uint8_t *)out)[i] = b->q.front();
    b->q.pop_front();
  }
  return k;
}
