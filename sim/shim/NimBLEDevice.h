// Bluetooth stand-in: scanning never finds an adapter, so the firmware shows
// "No OBD adapter found" unless the demo car is on (Settings > Car > OBD
// adapter > Demo car), exactly like a car without the Vgate plugged in.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "Arduino.h"

class NimBLEAddress {
public:
  std::string toString() const { return "00:00:00:00:00:00"; }
};

class NimBLEAdvertisedDevice {
public:
  const NimBLEAddress &getAddress() const { return addr_; }
  std::string getName() const { return ""; }

private:
  NimBLEAddress addr_;
};

class NimBLEScanResults {
public:
  int getCount() const { return 0; }
  const NimBLEAdvertisedDevice *getDevice(uint32_t) const { return nullptr; }
};

class NimBLEScan {
public:
  void setActiveScan(bool) {}
  void setInterval(uint16_t) {}
  void setWindow(uint16_t) {}
  NimBLEScanResults getResults(uint32_t ms, bool = false) {
    delay(ms);  // a real scan takes this long too
    return NimBLEScanResults();
  }
};

class NimBLERemoteCharacteristic {
public:
  typedef std::function<void(NimBLERemoteCharacteristic *, uint8_t *, size_t, bool)> notify_callback;
  bool canNotify() const { return false; }
  bool canIndicate() const { return false; }
  bool canWrite() const { return false; }
  bool canWriteNoResponse() const { return false; }
  bool subscribe(bool = true, const notify_callback = nullptr, bool = true) const { return false; }
  bool writeValue(const uint8_t *, size_t, bool = false) const { return false; }
};

class NimBLERemoteService {
public:
  const std::vector<NimBLERemoteCharacteristic *> &getCharacteristics(bool = false) const { return c_; }

private:
  std::vector<NimBLERemoteCharacteristic *> c_;
};

class NimBLEClient {
public:
  void setConnectTimeout(uint32_t) {}
  bool connect(const NimBLEAdvertisedDevice *, bool = true, bool = false, bool = true) { return false; }
  bool isConnected() { return false; }
  int disconnect() { return 0; }
  const std::vector<NimBLERemoteService *> &getServices(bool = false) { return s_; }

private:
  std::vector<NimBLERemoteService *> s_;
};

class NimBLEDevice {
public:
  static bool init(const std::string &) { return true; }
  static NimBLEScan *getScan() {
    static NimBLEScan s;
    return &s;
  }
  static NimBLEClient *createClient() { return new NimBLEClient(); }
};
