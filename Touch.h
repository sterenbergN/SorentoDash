// Minimal CST816D touch driver with swipe detection (polled over I2C).
#pragma once
#include <Arduino.h>
#include <Wire.h>

class Touch {
public:
  enum Gesture { NONE, SWIPE_LEFT, SWIPE_RIGHT, TAP };

  void begin(int sda, int scl, int rst) {
    pinMode(rst, OUTPUT);
    digitalWrite(rst, LOW);
    delay(10);
    digitalWrite(rst, HIGH);
    delay(50);
    Wire1.begin(sda, scl, 400000);
    write(0xFE, 0xFF);  // disable auto-sleep so polling always answers
  }

  Gesture poll(uint32_t now) {
    if (now - lastPoll < 15) return NONE;
    lastPoll = now;
    uint8_t d[5];
    if (!read(0x02, d, 5)) return NONE;
    bool down = (d[0] & 0x0F) > 0;
    int x = ((d[1] & 0x0F) << 8) | d[2];
    int y = ((d[3] & 0x0F) << 8) | d[4];
    if (down) {
      if (!touching) { touching = true; x0 = x; y0 = y; t0 = now; }
      x1 = x;
      y1 = y;
      return NONE;
    }
    if (!touching) return NONE;
    touching = false;
    int dx = x1 - x0, dy = y1 - y0;
    if (abs(dx) > 45 && abs(dx) > abs(dy) * 1.3f && now - t0 < 800) return dx < 0 ? SWIPE_LEFT : SWIPE_RIGHT;
    if (abs(dx) < 12 && abs(dy) < 12 && now - t0 < 400) return TAP;
    return NONE;
  }

private:
  static constexpr uint8_t ADDR = 0x15;
  uint32_t lastPoll = 0, t0 = 0;
  bool touching = false;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

  void write(uint8_t reg, uint8_t v) {
    Wire1.beginTransmission(ADDR);
    Wire1.write(reg);
    Wire1.write(v);
    Wire1.endTransmission();
  }
  bool read(uint8_t reg, uint8_t *buf, int n) {
    Wire1.beginTransmission(ADDR);
    Wire1.write(reg);
    if (Wire1.endTransmission(false)) return false;
    if (Wire1.requestFrom(ADDR, (uint8_t)n) != n) return false;
    for (int i = 0; i < n; i++) buf[i] = Wire1.read();
    return true;
  }
};
