#pragma once
#include "Arduino.h"
// OTA updates can't be simulated; the web server stand-in rejects uploads.
#define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF
#define U_FLASH 0
class UpdateClass {
public:
  bool begin(size_t, int = U_FLASH) { return false; }
  size_t write(const uint8_t *, size_t) { return 0; }
  bool end(bool = false) { return false; }
  void abort() {}
  bool isFinished() { return false; }
  bool hasError() { return true; }
  const char *errorString() { return "not available in the simulator"; }
};
inline UpdateClass Update;
