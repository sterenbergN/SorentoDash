// Event log for troubleshooting without a USB cable: restarts, adapter
// connections, drives, fill-ups, warnings and settings changes, one line each,
// kept in a text file on the flash and shown on the phone page.
//
// There's no clock, so each line is stamped with the restart number and the
// time since that restart: "#12 0:05:23  Adapter connected: OBDII".
// Safe to call from any task (the OBD task logs too).
#pragma once
#include <Arduino.h>
#include <FFat.h>
#include <Preferences.h>
#include <esp_system.h>
#include <stdarg.h>

class EventLog {
public:
  static constexpr size_t MAX_BYTES = 48 * 1024;  // then the file becomes the "old" half

  // Call once the FAT partition is mounted.
  void begin() {
    mtx = xSemaphoreCreateMutex();
    Preferences p;
    p.begin("log", false);
    boot = p.getUInt("boot", 0) + 1;
    p.putUInt("boot", boot);
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
    int n = snprintf(line, sizeof(line), "#%lu %lu:%02lu:%02lu  %s\n", (unsigned long)boot, (unsigned long)(s / 3600),
                     (unsigned long)(s / 60 % 60), (unsigned long)(s % 60), msg);
    Serial.print(line);
    if (!ready) return;
    lock();
    File f = FFat.open(CUR, FILE_APPEND);
    if (f) {
      f.write((const uint8_t *)line, min<int>(n, sizeof(line) - 1));
      size_t size = f.size();
      f.close();
      if (size > MAX_BYTES) {  // keep two halves: the previous one and this one
        FFat.remove(OLD);
        FFat.rename(CUR, OLD);
      }
    }
    unlock();
  }

  // Whole log, oldest first.
  String text() {
    String out;
    lock();
    for (const char *path : {OLD, CUR}) {
      File f = FFat.open(path, FILE_READ);
      if (!f) continue;
      size_t n = f.size();
      String part;
      part.reserve(n);
      char buf[512];
      size_t got;
      while ((got = f.read((uint8_t *)buf, sizeof(buf))) > 0) part.concat(buf, got);
      f.close();
      out += part;
    }
    unlock();
    return out;
  }

  void clear() {
    lock();
    FFat.remove(OLD);
    FFat.remove(CUR);
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
  SemaphoreHandle_t mtx = nullptr;
  bool ready = false;
  uint32_t boot = 0;

  void lock() { if (mtx) xSemaphoreTake(mtx, portMAX_DELAY); }
  void unlock() { if (mtx) xSemaphoreGive(mtx); }
};

inline EventLog events;
