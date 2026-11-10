#pragma once
#include "WiFi.h"
// Captive-portal DNS isn't needed on a PC.
class DNSServer {
public:
  bool start(uint16_t, const char *, IPAddress) { return true; }
  void stop() {}
  void processNextRequest() {}
};
