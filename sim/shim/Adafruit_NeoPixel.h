// LED ring stand-in: show() hands the colours to the simulator window.
#pragma once
#include <vector>
#include "Arduino.h"

#define NEO_GRB 0x52
#define NEO_KHZ800 0x0000

class Adafruit_NeoPixel {
public:
  Adafruit_NeoPixel(uint16_t n, int16_t, uint16_t) : px_(n, 0) {}
  void begin() {}
  void setBrightness(uint8_t b) { bright_ = b; }
  void clear() { std::fill(px_.begin(), px_.end(), 0); }
  void setPixelColor(uint16_t i, uint8_t r, uint8_t g, uint8_t b) {
    if (i < px_.size()) px_[i] = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
  }
  void show() {
    std::vector<uint32_t> out(px_.size());
    for (size_t i = 0; i < px_.size(); i++) {
      uint32_t c = px_[i];
      auto s = [&](int sh) { return (((c >> sh) & 0xFF) * (bright_ + 1) >> 8) << sh; };
      out[i] = s(16) | s(8) | s(0);
    }
    sim::setLeds(out.data(), (int)out.size());
  }

private:
  std::vector<uint32_t> px_;
  uint8_t bright_ = 255;
};
