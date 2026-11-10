// Small UI types shared by the sketch (kept in a header so the Arduino
// prototype generator sees them before any function that uses them).
#pragma once
#include <Arduino.h>
#include <functional>

enum Press { PRESS_NONE, PRESS_SHORT, PRESS_LONG, PRESS_DOUBLE, PRESS_REPEAT };

enum Mode { MODE_HORNS, MODE_CAR, MODE_MENU, MODE_VOLUME, MODE_BRIGHTNESS, MODE_WIFI, MODE_FILLUP, MODE_NUMBER, MODE_CONFIRM, MODE_HISTORY, MODE_DETAIL, MODE_OTA, MODE_TEXT };

// A scrolling list position: `target` is unbounded and wraps via modulo,
// `pos` eases toward it for the drum animation.
struct Wheel {
  int32_t target = 0;
  float pos = 0;
  int index(int n) const { return n ? ((target % n) + n) % n : 0; }
  void jump(int i) { target = i; pos = i; }
  void animate(float dt) {
    pos += (target - pos) * min(1.0f, dt * 14);
    if (fabsf(target - pos) < 0.002f) pos = target;
  }
};
