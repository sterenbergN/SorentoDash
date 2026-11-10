// Wi-Fi stand-in: the access point is pretend; the web server listens on
// http://localhost:8080 instead. A "phone" counts as connected while a
// browser has made requests recently.
#pragma once
#include "Arduino.h"

#define WIFI_AP 2
#define WIFI_OFF 0

class IPAddress {
public:
  IPAddress(uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0) : v{a, b, c, d} {}
  uint8_t v[4];
};

class WiFiClass {
public:
  bool mode(int) { return true; }
  bool softAPConfig(IPAddress, IPAddress, IPAddress) { return true; }
  bool softAP(const char *, const char * = nullptr) { return true; }
  bool softAPdisconnect(bool = false) { return true; }
  int softAPgetStationNum() { return sim::recentWebRequest() ? 1 : 0; }
};
inline WiFiClass WiFi;
