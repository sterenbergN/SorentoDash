// I2C stand-in: only the CST816D touch controller (address 0x15) answers.
#pragma once
#include "Arduino.h"

class TwoWire {
public:
  bool begin(int = -1, int = -1, uint32_t = 0) { return true; }
  void beginTransmission(uint8_t addr) { addr_ = addr; n_ = 0; }
  size_t write(uint8_t b) {
    if (n_ == 0) reg_ = b;
    n_++;
    return 1;
  }
  uint8_t endTransmission(bool = true) { return addr_ == 0x15 ? 0 : 2; }
  uint8_t requestFrom(uint8_t addr, uint8_t n) {
    if (addr != 0x15 || n > sizeof(buf_)) return 0;
    len_ = sim::touchRead(reg_, buf_, n);
    pos_ = 0;
    return (uint8_t)len_;
  }
  int available() { return len_ - pos_; }
  int read() { return pos_ < len_ ? buf_[pos_++] : -1; }

private:
  uint8_t addr_ = 0, reg_ = 0, buf_[16];
  int n_ = 0, len_ = 0, pos_ = 0;
};

inline TwoWire Wire, Wire1;
