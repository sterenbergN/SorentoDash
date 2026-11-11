// Event log for troubleshooting without a USB cable: restarts, adapter
// connections, drives, fill-ups, warnings and settings changes, one line each,
// kept in a text file on the flash and shown on the phone page.
//
// There's no clock, so each line is stamped with the restart number and the
// time since that restart: "#12 0:05:23  Adapter connected: OBDII".
// Safe to call from any task (the OBD task logs too).
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <stdarg.h>
#include "Storage.h"

class EventLog {
public:
  static constexpr size_t MAX_BYTES = 48 * 1024;  // then the file becomes the "old" half
  // Nothing is written to flash in the first seconds after power-up: in the car
  // the power drops again about a second later while the starter cranks. Lines
  // wait in memory until then, and a start that short isn't counted as a restart.
  static constexpr uint32_t HOLD_MS = 5000;

  // Call once the file store is mounted.
  void begin() {
    mtx = xSemaphoreCreateMutex();
    Preferences p;
    p.begin("log", true);
    boot = p.getUInt("boot", 0) + 1;  // saved at the first flush
    p.end();
    ready = true;
  }

  uint32_t bootNumber() const { return boot; }

  void add(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    uint32_t s = millis() / 1000;
    char line[240];
    snprintf(line, sizeof(line), "#%lu %lu:%02lu:%02lu  %s\n", (unsigned long)boot, (unsigned long)(s / 3600),
             (unsigned long)(s / 60 % 60), (unsigned long)(s % 60), msg);
    Serial.print(line);
    if (!ready) return;
    lock();
    if (pending.length() < MAX_PENDING) pending += line;
    unlock();
    poll();
  }

  // Write the waiting lines once it's safe to. Call regularly (main loop).
  void poll() {
    if (!ready || millis() < HOLD_MS || (retryAt && millis() < retryAt)) return;
    lock();
    if (!counted) {
      Preferences p;
      p.begin("log", false);
      p.putUInt("boot", boot);
      p.end();
      counted = true;
    }
    if (pending.length()) {
      if (Storage::appendFile(CUR, pending.c_str(), pending.length())) {
        pending = "";
        retryAt = 0;
        if (Storage::size(CUR) > MAX_BYTES) {  // keep two halves: the previous one and this one
          LittleFS.remove(OLD);
          LittleFS.rename(CUR, OLD);
        }
      } else {
        retryAt = millis() + 15000;  // the store isn't taking writes: don't hammer it
        if (!retryAt) retryAt = 1;
      }
    }
    unlock();
  }

  // Whole log, oldest first (including lines not yet written to flash).
  String text() {
    String out;
    lock();
    for (const char *path : {OLD, CUR}) {
      std::vector<uint8_t> part = Storage::readFile(path);
      if (!part.empty()) out.concat((const char *)part.data(), part.size());
    }
    out += pending;
    unlock();
    return out;
  }

  void clear() {
    lock();
    Storage::remove(OLD);
    Storage::remove(CUR);
    pending = "";
    unlock();
  }

  // Why the chip last started, in plain words.
  static const char *resetReason() {
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON: return "power on";
      case ESP_RST_SW: return "restart";
      case ESP_RST_PANIC: return "CRASH (software error)";
      case ESP_RST_INT_WDT:
      case ESP_RST_TASK_WDT:
      case ESP_RST_WDT: return "CRASH (froze, watchdog)";
      case ESP_RST_BROWNOUT: return "power dip (brownout)";
      case ESP_RST_EXT: return "reset pin";
      case ESP_RST_DEEPSLEEP: return "wake from sleep";
      default: return "unknown";
    }
  }

private:
  static constexpr const char *CUR = "/events.log";
  static constexpr const char *OLD = "/events.old";
  static constexpr size_t MAX_PENDING = 6000;  // lines kept in memory while the store can't take them
  SemaphoreHandle_t mtx = nullptr;
  bool ready = false, counted = false;
  uint32_t boot = 0, retryAt = 0;
  String pending;

  void lock() { if (mtx) xSemaphoreTake(mtx, portMAX_DELAY); }
  void unlock() { if (mtx) xSemaphoreGive(mtx); }
};

inline EventLog events;
