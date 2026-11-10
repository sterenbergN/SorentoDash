// Desktop stand-in for the Arduino-ESP32 core: just the parts Sorento Dash
// uses, routed to the simulated board (sim/SimHardware.h).
#pragma once
#ifdef _WIN32
// Windows headers first: Arduino's INPUT/OUTPUT/HIGH... macros below would
// otherwise clash with names inside them (the web server needs winsock).
#include <winsock2.h>
#include <ws2tcpip.h>
#undef DOUBLE_CLICK  // Windows console macro; LovyanGFX uses the name in an enum
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include "../SimHardware.h"
#include "WString.h"

using std::max;
using std::min;

#define IRAM_ATTR
#define DRAM_ATTR
// PROGMEM, memcpy_P, pgm_read_*: LovyanGFX's desktop pgmspace.h provides them.
#include <lgfx/utility/pgmspace.h>

#ifndef PI
#define PI 3.14159265358979323846
#endif
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105

#define HIGH 1
#define LOW 0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define CHANGE 0x03
#define RISING 0x01
#define FALLING 0x02
#define SERIAL_8N1 0x800001c

template <class T, class L, class H>
inline T constrain(T v, L lo, H hi) { return v < (T)lo ? (T)lo : v > (T)hi ? (T)hi : v; }
inline long map(long x, long in0, long in1, long out0, long out1) { return (x - in0) * (out1 - out0) / (in1 - in0) + out0; }

// ---- time
inline unsigned long micros() {
  static const auto t0 = std::chrono::steady_clock::now();
  return (unsigned long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}
inline unsigned long millis() { return micros() / 1000; }
inline void delay(unsigned long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline void delayMicroseconds(unsigned int us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
inline void yield() {}

// ---- GPIO / interrupts / PWM
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int v) { sim::writePin(pin, v); }
inline int digitalRead(int pin) { return sim::readPin(pin); }
inline int digitalPinToInterrupt(int pin) { return pin; }
inline void attachInterruptArg(int pin, void (*fn)(void *), void *arg, int) { sim::attachIsr(pin, fn, arg); }
namespace sim_detail {
inline void callVoid(void *fn) { ((void (*)())fn)(); }
}
inline void attachInterrupt(int pin, void (*fn)(), int) { sim::attachIsr(pin, sim_detail::callVoid, (void *)fn); }
inline bool ledcAttach(int, uint32_t, uint8_t) { return true; }
inline bool ledcWrite(int pin, uint32_t v) { sim::setPwm(pin, v); return true; }

// ---- strings
inline size_t strlcpy(char *dst, const char *src, size_t n) {
  size_t len = strlen(src);
  if (n) {
    size_t c = len >= n ? n - 1 : len;
    memcpy(dst, src, c);
    dst[c] = 0;
  }
  return len;
}

// ---- Print / Stream
class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) write(buf[i]);
    return n;
  }
  size_t write(const char *s) { return write((const uint8_t *)s, strlen(s)); }
  size_t print(const char *s) { return write(s); }
  size_t print(const String &s) { return write(s.c_str()); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v) { return print(String(v)); }
  size_t print(unsigned v) { return print(String(v)); }
  size_t print(long v) { return print(String(v)); }
  size_t print(unsigned long v) { return print(String(v)); }
  size_t print(double v, int d = 2) { return print(String(v, (unsigned char)d)); }
  size_t println() { return write("\r\n"); }
  template <class T>
  size_t println(const T &v) { size_t n = print(v); return n + println(); }
  size_t printf(const char *f, ...) {
    char b[1024];
    va_list a;
    va_start(a, f);
    int n = vsnprintf(b, sizeof(b), f, a);
    va_end(a);
    return write((const uint8_t *)b, (size_t)std::min(n, (int)sizeof(b) - 1));
  }
  virtual void flush() {}
};

class Stream : public Print {
public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() { return -1; }
  void setTimeout(unsigned long ms) { timeout = ms; }
  String readStringUntil(char term) {
    String s;
    unsigned long t0 = millis();
    while (millis() - t0 < timeout) {
      if (!available()) { delay(1); continue; }
      int c = read();
      if (c < 0 || c == term) break;
      s += (char)c;
    }
    return s;
  }

protected:
  unsigned long timeout = 1000;
};

// UART1 -> the simulated Audio FX sound board.
class HardwareSerial : public Stream {
public:
  void begin(unsigned long, uint32_t = SERIAL_8N1, int = -1, int = -1) { sim::soundboardOpen(true); }
  void end() { sim::soundboardOpen(false); }
  size_t write(uint8_t c) override { sim::soundboardWrite(c); return 1; }
  using Print::write;
  int available() override { return sim::soundboardAvailable(); }
  int read() override { return sim::soundboardRead(); }
};

// USB CDC console -> stdout / stdin.
class HWCDC : public Stream {
public:
  void begin(unsigned long = 0) {}
  void setTxTimeoutMs(uint32_t) {}
  static bool isPlugged() { return false; }  // like the car: not on a computer
  size_t write(uint8_t c) override { sim::consoleWrite(&c, 1); return 1; }
  size_t write(const uint8_t *b, size_t n) override { sim::consoleWrite(b, n); return n; }
  using Print::write;
  int available() override { return sim::consoleAvailable(); }
  int read() override { return sim::consoleRead(); }
  operator bool() const { return true; }
};

extern HWCDC Serial;
extern HardwareSerial Serial1;

// ---- ESP
class EspClass {
public:
  uint64_t getEfuseMac() { return 0x5A1B2C3D4E5FULL; }
  [[noreturn]] void restart() {
    fflush(stdout);
    std::exit(3);  // run.ps1 restarts the simulator on this exit code
  }
};
extern EspClass ESP;

#include "freertos/FreeRTOS.h"

// The sketch's entry points.
void setup();
void loop();
