// OBD-II over a Bluetooth LE ELM327 adapter (e.g. Vgate iCar Pro BLE), plus a
// trip computer (MPG, fuel used, range) and trouble-code reading/clearing.
//
// Runs in its own FreeRTOS task because BLE scanning/connecting and the slow
// K-line protocol (2006 Sorento: ISO 14230 KWP2000) block for seconds at a
// time. The UI reads a snapshot with obd.snapshot().
//
// A built-in simulated adapter ("demo car") answers the same ELM327 commands
// so the whole pipeline can be exercised without a car.
#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <freertos/stream_buffer.h>
#include <math.h>
#include "EventLog.h"
#include "Health.h"
#include "History.h"

struct TripTotals {
  float miles = 0, gallons = 0, seconds = 0;
  float mpg() const { return gallons > 0.02f ? miles / gallons : -1; }
};

struct ObdData {
  enum State : uint8_t { NOT_FOUND, SEARCHING, CONNECTING, INITIALIZING, NO_ECU, LIVE };
  State state = SEARCHING;
  char adapter[32] = "";
  bool simulated = false;
  bool useMaf = false;

  float mph = NAN, rpm = NAN, coolantF = NAN, intakeF = NAN, volts = NAN, loadPct = NAN;
  // Detail readings: fuel trims (%, bank 1/2), ignition timing (deg), throttle (%).
  float stft[2] = {NAN, NAN}, ltft[2] = {NAN, NAN};
  float timingDeg = NAN, throttlePct = NAN;
  float gph = NAN;     // fuel flow, smoothed
  float instMpg = NAN; // smoothed; NAN when stopped

  bool milOn = false;
  bool dtcsRead = false;
  int nDtcs = 0;
  uint16_t dtcs[8];

  // Transmission computer's own codes, read on request (Obd::readTransmission).
  enum TransState : uint8_t { TR_NOT_READ, TR_READING, TR_DONE, TR_NO_ANSWER, TR_REFUSED, TR_MOVING, TR_CAR_OFF };
  TransState trans = TR_NOT_READ;
  uint8_t transAddr = 0;  // where it answered on the K-line
  int nTransDtcs = 0;
  uint16_t transDtcs[8];
  bool transNow[8];  // fault present right now (otherwise stored from earlier)

  String transText() const {
    switch (trans) {
      case TR_READING: return "checking...";
      case TR_DONE: return nTransDtcs ? String(nTransDtcs) + (nTransDtcs == 1 ? " code" : " codes") : String("no codes");
      case TR_NO_ANSWER: return "no answer";
      case TR_REFUSED: return "answered, no code list";
      case TR_MOVING: return "stop the car first";
      case TR_CAR_OFF: return "turn the ignition on first";
      default: return "not checked";
    }
  }

  TripTotals trip, tank;
  TripTotals drive;  // since the car was started (not saved)
  float odoMiles = 0;  // lifetime miles tracked by the display (for service reminders)
  float tankCapacity = 21.1f;
  bool tankFromFull = false;  // tank level is known: a logged fill-up or a set fuel level
  float tankStartUsed = 0;    // gallons already gone when tracking started (0 = from full)
  bool tankIncomplete = false;  // some driving since the fill-up wasn't tracked
  float fuelCal = 1.0f;       // fuel-flow correction learned from fill-ups

  // What the last logged fill-up did to the calibration (for the UI).
  enum FillResult : uint8_t { FILL_NONE, FILL_CALIBRATED, FILL_STARTED, FILL_UNTRACKED, FILL_TOO_DIFFERENT, FILL_TOO_SMALL };
  int calFills = 0;               // fill-ups that have adjusted the calibration
  float lastFillEstimate = 0;     // gallons the display counted for that tank
  float lastFillPumped = 0;       // gallons the pump said
  FillResult lastFill = FILL_NONE;
  uint32_t updatedAt = 0;

  // Fresh data (alerts use this). Screens just need `state == LIVE`: the
  // K-line is slow, so a round of readings can occasionally take a while.
  bool live() const { return state == LIVE && millis() - updatedAt < 15000; }

  // Fuel gone from the tank: what was already used when tracking started
  // (from a set fuel level) plus what's been counted since.
  float tankUsed() const { return tankStartUsed + tank.gallons; }

  // Miles left in the tank. Unknown (NAN) until the level is known: a logged
  // fill-up, or a fuel level set from the gauge.
  float rangeMiles() const {
    if (!tankFromFull) return NAN;
    float mpg = tank.miles > 20 ? tank.mpg() : trip.miles > 5 ? trip.mpg() : 18.0f;
    if (mpg <= 0) mpg = 18.0f;
    return max(0.0f, (tankCapacity - tankUsed()) * mpg);
  }
  float tankFraction() const { return constrain(1.0f - tankUsed() / tankCapacity, 0.0f, 1.0f); }

  const char *stateText() const {
    switch (state) {
      case NOT_FOUND: return "No OBD adapter found";
      case SEARCHING: return "Looking for adapter";
      case CONNECTING: return "Connecting";
      case INITIALIZING: return "Starting adapter";
      case NO_ECU: return "Waiting for ignition";
      default: return "Connected";
    }
  }

  // "P0171"
  static String dtcText(uint16_t c) {
    static const char L[] = {'P', 'C', 'B', 'U'};
    char b[6];
    snprintf(b, sizeof(b), "%c%X%X%X%X", L[c >> 14], (c >> 12) & 3, (c >> 8) & 0xF, (c >> 4) & 0xF, c & 0xF);
    return b;
  }
};

// ---------------------------------------------------------------- links
class ElmLink {
public:
  StreamBufferHandle_t rx = nullptr;
  ElmLink() { rx = xStreamBufferCreate(1024, 1); }
  virtual ~ElmLink() {}
  virtual bool open(char *name, size_t n) = 0;  // blocking
  virtual bool isOpen() = 0;
  virtual void close() = 0;
  virtual void write(const char *cmd) = 0;  // cmd without the trailing CR
  virtual String address() { return ""; }
};

class BleElm : public ElmLink {
public:
  String preferred;  // remembered adapter address

  bool open(char *name, size_t n) override {
    if (!inited) {
      NimBLEDevice::init("SorentoDash");
      inited = true;
    }
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    // Scan for up to 4 s, but stop the moment the remembered adapter shows up
    // (usually well under a second) instead of always waiting the full time.
    strlcpy(watcher.want, preferred.c_str(), sizeof(watcher.want));
    scan->setScanCallbacks(&watcher, false);
    NimBLEScanResults res = scan->getResults(4000, false);
    const NimBLEAdvertisedDevice *pick = nullptr;
    for (int i = 0; i < res.getCount(); i++) {
      const NimBLEAdvertisedDevice *d = res.getDevice(i);
      if (preferred.length() && preferred == d->getAddress().toString().c_str()) { pick = d; break; }
      if (!pick && looksLikeObd(d->getName().c_str())) pick = d;
    }
    if (!pick) return false;
    strlcpy(name, pick->getName().length() ? pick->getName().c_str() : "OBD adapter", n);

    if (!client) {
      client = NimBLEDevice::createClient();
      client->setConnectTimeout(6000);
    }
    if (!client->connect(pick)) return false;
    addr = pick->getAddress().toString().c_str();

    // Find a service with a notify characteristic (adapter -> us) and a
    // writable one (us -> adapter). Vendors use different UUIDs.
    NimBLERemoteCharacteristic *notifyC = nullptr, *writeC = nullptr;
    for (auto *s : client->getServices(true)) {
      NimBLERemoteCharacteristic *nC = nullptr, *wC = nullptr;
      for (auto *c : s->getCharacteristics(true)) {
        if (!nC && (c->canNotify() || c->canIndicate())) nC = c;
        if (!wC && (c->canWrite() || c->canWriteNoResponse())) wC = c;
      }
      if (nC && wC) { notifyC = nC; writeC = wC; break; }
    }
    if (!notifyC || !writeC) { client->disconnect(); return false; }
    StreamBufferHandle_t buf = rx;
    notifyC->subscribe(notifyC->canNotify(), [buf](NimBLERemoteCharacteristic *, uint8_t *d, size_t len, bool) {
      xStreamBufferSend(buf, d, len, 0);
    });
    wr = writeC;
    return true;
  }

  bool isOpen() override { return client && client->isConnected() && wr; }
  void close() override {
    if (client && client->isConnected()) client->disconnect();
    wr = nullptr;
  }
  void write(const char *cmd) override {
    if (!isOpen()) return;
    String s = String(cmd) + "\r";
    wr->writeValue((const uint8_t *)s.c_str(), s.length(), !wr->canWriteNoResponse());
  }
  String address() override { return addr; }

private:
  // Ends the scan early once the remembered adapter has been heard.
  struct Watcher : NimBLEScanCallbacks {
    char want[24] = "";
    void onResult(const NimBLEAdvertisedDevice *d) override {
      if (want[0] && !strcmp(want, d->getAddress().toString().c_str())) NimBLEDevice::getScan()->stop();
    }
  } watcher;
  bool inited = false;
  NimBLEClient *client = nullptr;
  NimBLERemoteCharacteristic *wr = nullptr;
  String addr;

  static bool looksLikeObd(String n) {
    n.toUpperCase();
    for (const char *k : {"OBD", "ELM", "VLINK", "V-LINK", "ICAR", "VEEPEAK", "KONNWEI", "VGATE", "CARLY", "SCAN"})
      if (n.indexOf(k) >= 0) return true;
    return false;
  }
};

// A pretend ELM327 + ECU: a 60 s drive cycle, a warming engine and one stored
// trouble code, answering exactly like the real thing (spaces/echo off).
class SimElm : public ElmLink {
public:
  bool open(char *name, size_t n) override {
    strlcpy(name, "Demo car", n);
    if (!t0) t0 = millis();  // the demo car keeps "driving" while disconnected
    opened = true;
    codes = true;
    return true;
  }
  bool isOpen() override { return opened; }
  void close() override { opened = false; }
  float extraMiles = 0;  // pretend driving done while the display wasn't watching
#ifdef SORENTO_SIM
  static inline bool trace = false;  // simulator console "elmtrace 1": print every adapter command
#endif
  void write(const char *cmd) override {
    String c(cmd);
    c.toUpperCase();
    c.replace(" ", "");
    String r = respond(c);
    // Headers on: "83 F1 11 <data> <checksum>", as the engine computer (0x11) would send.
    if (headers && target == OBD_ALL && !c.startsWith("AT") && r.startsWith("4"))
      r = hex2(0x80 | (r.length() / 2)) + "F111" + r + "00";
#ifdef SORENTO_SIM
    if (trace) Serial.printf("[elm] %s -> %s\n", c.c_str(), r.c_str());
#endif
    vTaskDelay(pdMS_TO_TICKS(c.startsWith("AT") ? 5 : 45));  // K-line is slow
    r += "\r\r>";
    xStreamBufferSend(rx, r.c_str(), r.length(), 0);
  }

private:
  static constexpr uint8_t OBD_ALL = 0x33;  // standard requests go to every computer
  static constexpr uint8_t TCM = 0x18;      // the demo car's transmission computer
  bool opened = false, codes = true;
  bool headers = false, tcmAwake = false, tcmSession = false;
  uint8_t target = OBD_ALL;
  uint32_t t0 = 0;

  // The adapter's own commands. Requests go to whichever computer ATSH names.
  String adapter(const String &c) {
    if (c == "ATZ" || c == "ATWS") {
      target = OBD_ALL;
      headers = tcmAwake = tcmSession = false;
      return "ELM327 v1.5";
    }
    if (c == "ATRV") return "14.1V";
    if (c == "ATDPN") return "A5";
    if (c == "ATH1" || c == "ATH0") headers = c == "ATH1";
    if (c.startsWith("ATSH") && c.length() >= 10) target = strtol(c.substring(6, 8).c_str(), nullptr, 16);
    if (c == "ATPC") tcmAwake = tcmSession = false;
    if (c == "ATFI" || c == "ATSI") {
      tcmAwake = target == TCM;
      tcmSession = false;
      return target == TCM || target == OBD_ALL ? "BUS INIT: OK" : "BUS INIT: ...ERROR";
    }
    return "OK";
  }

  // A pretend transmission computer with two stored codes. Like many real
  // ones, it wants a diagnostic session opened before it lists them.
  String transmission(const String &c) {
    if (!tcmAwake) return "NO DATA";
    if (c == "1081") { tcmSession = true; return "5081"; }
    if (c == "1800FF00") return tcmSession ? "5802071560074120" : "7F1880";  // P0715 now, P0741 stored
    if (c == "3E") return "7E";
    if (c == "82") { tcmAwake = false; return "C2"; }
    return "7F" + c.substring(0, 2) + "11";
  }

  // Speed (mph) over a repeating 60 s cycle.
  static float speedAt(float t) {
    t = fmodf(t, 60);
    if (t < 8) return 0;
    if (t < 20) return 45 * (t - 8) / 12;
    if (t < 40) return 45 + 3 * sinf(t);
    if (t < 48) return 45 * (48 - t) / 8;
    return 0;
  }

  static String hex2(int v) {
    char b[3];
    snprintf(b, sizeof(b), "%02X", constrain(v, 0, 255));
    return b;
  }

  String respond(const String &c) {
    if (c.startsWith("AT")) return adapter(c);
    if (target == TCM) return transmission(c);
    if (target != OBD_ALL) return "NO DATA";
    float t = (millis() - t0) / 1000.0f;
    float mph = speedAt(t), accel = speedAt(t + 0.5f) - speedAt(t - 0.5f);
    int kph = lroundf(mph * 1.609f);
    int rpm = 750 + mph * 38 + max(0.0f, accel) * 300;
    float maf = 4 + mph * 0.45f + max(0.0f, accel) * 9;
    int coolantC = min(90.0f, 25 + t * 0.8f);
    if (c == "0100") return "4100BFBF8000";  // PIDs 01 03-09 0B-11
    if (c == "0101") return "4101" + hex2(codes ? 0x81 : 0) + "076500";
    if (c == "0104") return "4104" + hex2(40 + mph);
    if (c == "0105") return "4105" + hex2(coolantC + 40);
    if (c == "010B") return "410B" + hex2(30 + mph * 0.8f);
    if (c == "010C") return "410C" + hex2((rpm * 4) >> 8) + hex2((rpm * 4) & 0xFF);
    if (c == "010D") return "410D" + hex2(kph);
    if (c == "010F") return "410F" + hex2(22 + 40);
    if (c == "0110") {
      int v = maf * 100;
      return "4110" + hex2(v >> 8) + hex2(v & 0xFF);
    }
    if (c == "0106") return "4106" + hex2(128 + 3 + (int)(3 * sinf(t)));   // STFT B1 ~ +2%
    if (c == "0107") return "4107" + hex2(128 + 9);                        // LTFT B1 +7%
    if (c == "0108") return "4108" + hex2(128 + 2 + (int)(3 * sinf(t + 1)));
    if (c == "0109") return "4109" + hex2(128 + 11);                       // LTFT B2 +8.6%
    if (c == "010E") return "410E" + hex2((int)((12 + mph * 0.3f + 64) * 2));
    if (c == "0111") return "4111" + hex2(30 + mph * 1.2f + max(0.0f, accel) * 20);
    if (c == "03") return codes ? "43017100000000" : "43000000000000";
    if (c == "04") { codes = false; return "44"; }
    return "NODATA";
  }
};

// ---------------------------------------------------------------- manager
class Obd {
public:
  void begin() {
    prefs.begin("obd", false);
    data.trip = loadTrip("t");
    data.tank = loadTrip("k");
    data.odoMiles = prefs.getFloat("odo", 0);
    data.tankCapacity = prefs.getFloat("tankcap", 21.1f);
    data.tankFromFull = prefs.getBool("kfull", false);
    data.tankStartUsed = prefs.getFloat("kstart", 0);
    data.tankIncomplete = prefs.getBool("kinc", false);
    displacement = prefs.getFloat("disp", 3.5f);
    fuelCal = prefs.getFloat("cal", 1.0f);
    distCal = prefs.getFloat("dcal", 1.0f);
    odoGap = prefs.getBool("ogap", false);
    proto = prefs.getUChar("proto", 0);
    if (!isxdigit(proto)) proto = 0;
    loadCalInfo(data);
    simulate = prefs.getBool("sim", false);
    readOnly = prefs.getBool("ro", true);
    ble.preferred = prefs.getString("addr", "");
    mtx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore([](void *p) { ((Obd *)p)->run(); }, "obd", 8192, this, 1, nullptr, 0);
  }

  ObdData snapshot() {
    xSemaphoreTake(mtx, portMAX_DELAY);
    ObdData d = data;
    xSemaphoreGive(mtx);
    return d;
  }

  // Requests from the UI / phone page (handled by the OBD task).
  void resetTrip() { reqResetTrip = true; }
  void filledUp(float gallonsPumped) { reqFillUp = gallonsPumped; }
  // Replace the fuel calibration and re-scale the gallons counted with the old
  // one since the last fill-up (repairs a bad calibration).
  void setCalibration(float c) { reqCal = constrain(c, 0.5f, 2.0f); }
  // Back to the uncalibrated default and forget the fill-up history.
  void resetCalibration() { reqCalReset = true; }
  // Declare the tank tracked from full (e.g. after a fill-up logged before this existed).
  void markTankFull() { reqTankFull = true; }
  // Start tank tracking now from the fuel gauge (percent full) instead of
  // waiting for a fill-up. Being an estimate, the next fill-up won't calibrate.
  void setFuelLevel(float pct) { reqLevel = constrain(pct, 0.0f, 100.0f) + 1; }
  // The "some driving wasn't tracked" flag is wrong - clear it.
  void markTankComplete() { reqTankComplete = true; }
  // Tell the task when the display is plugged into a computer (bench testing
  // isn't unmonitored driving).
  void setOnComputer(bool on) { onComputer = on; }
  // Demo car: pretend it was driven this far while disconnected.
  void simDrive(float miles) { sim.extraMiles += miles; }
#ifdef SORENTO_SIM
  // Simulator testing: pretend the display counted this many more miles.
  void addTestMiles(float mi) {
    lock();
    data.odoMiles += mi;
    unlock();
  }
#endif
  // Ignored in read-only mode: clearing codes also wipes the engine
  // computer's learned idle and fuel trims.
  void clearCodes() {
    if (!readOnly) reqClear = true;
  }
  // Ask the transmission computer for its own codes. Takes several seconds
  // with no other readings, so it only runs on request with the car stopped.
  void readTransmission() { reqTrans = true; }
  void setReadOnly(bool on) {
    readOnly = on;
    prefs.putBool("ro", on);
    events.add("Read-only mode %s", on ? "on" : "off");
  }
  bool isReadOnly() const { return readOnly; }
  // While a details screen is open, poll the detail readings every 2 s
  // instead of every 10 s.
  void setDetailFocus(bool on) { detailFocus = on; }
  void setSimulate(bool on) {
    if (on != simulate) events.add("Demo car %s", on ? "on" : "off");
    simulate = on;
    prefs.putBool("sim", on);
  }
  void setDisplacement(float l) {
    displacement = constrain(l, 1.0f, 8.0f);
    prefs.putFloat("disp", displacement);
    if (health) health->setDisplacement(displacement);
    events.add("Engine size set to %.1f L", (float)displacement);
  }
  void setTankCapacity(float g) {
    lock();
    data.tankCapacity = constrain(g, 5.0f, 60.0f);
    unlock();
    prefs.putFloat("tankcap", data.tankCapacity);
    events.add("Tank size set to %.1f gal", data.tankCapacity);
  }
  // Where drives and fill-ups are recorded for the phone page's charts.
  void setHistory(History *h) { hist = h; }
  // Where readings go for the engine health checks.
  void setHealth(Health *h) {
    health = h;
    h->setDisplacement(displacement);
  }

  void forgetAdapter() {
    prefs.remove("addr");
    ble.preferred = "";
    reqReconnect = true;
    events.add("Adapter forgotten");
  }

  bool simulating() const { return simulate; }
  float displacementL() const { return displacement; }
  float calibration() const { return fuelCal; }

  // Speed correction: every speed reading is multiplied by this (tire size
  // and wear make the car's speed signal read a little high or low). Learned
  // from odometer corrections, or set by hand.
  float distanceCal() const { return distCal; }
  void setDistanceCal(float c) {
    distCal = constrain(c, 0.85f, 1.15f);
    prefs.putFloat("dcal", distCal);
  }
  // Some driving may have gone uncounted since the odometer was last set
  // (5+ min without the adapter), so the next correction can't teach the speed.
  bool distanceGap() const { return odoGap; }
  void clearDistanceGap() {
    odoGap = false;
    prefs.putBool("ogap", false);
  }

private:
  Preferences prefs;
  SemaphoreHandle_t mtx;
  ObdData data;
  BleElm ble;
  SimElm sim;
  ElmLink *link = nullptr;
  History *hist = nullptr;
  Health *health = nullptr;

  void endDrive() {
    if (hist) hist->endDrive();
    if (health) health->endDrive();
  }

  volatile bool simulate = false, reqResetTrip = false, reqClear = false, reqReconnect = false, reqTankFull = false;
  volatile bool reqTankComplete = false, onComputer = false, readOnly = true, detailFocus = false, reqTrans = false;
  uint32_t lastDetail = 0;

  // Car distance counter reference, taken whenever we are in sync with it.
  volatile bool reqCalReset = false;
  uint32_t unmonitoredSince = 0;  // display on, adapter missing (see watchForGaps)
  volatile float reqFillUp = 0, reqCal = 0, reqLevel = 0;  // reqLevel: percent + 1 (0 = none)
  volatile float displacement = 3.5f, fuelCal = 1.0f, distCal = 1.0f;
  bool odoGap = false;
  char proto = 0;  // adapter protocol number the car answered on last time ('5'), 0 = not known

  uint32_t supported[3] = {0, 0, 0};
  uint32_t lastFast = 0, lastSlow = 0, lastMil = 0, lastSave = 0, lastLive = 0;
  int fails = 0;
  float mphEma = NAN, gphEma = NAN, mapKpa = NAN;

  void lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
  void unlock() { xSemaphoreGive(mtx); }
  bool missingLogged = false;  // "adapter not found" is logged once until it's found

  void setState(ObdData::State s) {
    lock();
    ObdData::State was = data.state;
    data.state = s;
    unlock();
    if (s != was && !data.simulated) logState(was, s);
  }

  // Connection changes worth a line in the event log (not every search retry).
  void logState(ObdData::State was, ObdData::State s) {
    using S = ObdData;
    if (s == S::LIVE) events.add("Car on: engine computer answering");
    else if (s == S::NO_ECU && was == S::LIVE) events.add("Car off: engine computer stopped answering");
    else if (s == S::NO_ECU && was == S::INITIALIZING) {
      events.add("Adapter connected: %s", data.adapter[0] ? data.adapter : "?");
      missingLogged = false;
    } else if (s == S::SEARCHING && (was == S::LIVE || was == S::NO_ECU)) events.add("Adapter connection lost");
    else if (s == S::NOT_FOUND && !missingLogged) {
      events.add("Adapter not found (still looking)");
      missingLogged = true;
    }
  }

  TripTotals loadTrip(const char *p) {
    TripTotals t;
    t.miles = prefs.getFloat((String(p) + "mi").c_str(), 0);
    t.gallons = prefs.getFloat((String(p) + "gal").c_str(), 0);
    t.seconds = prefs.getFloat((String(p) + "sec").c_str(), 0);
    return t;
  }
  void saveTrip(const char *p, const TripTotals &t) {
    prefs.putFloat((String(p) + "mi").c_str(), t.miles);
    prefs.putFloat((String(p) + "gal").c_str(), t.gallons);
    prefs.putFloat((String(p) + "sec").c_str(), t.seconds);
  }
  void saveTrips() {
    if (data.simulated) return;  // demo-car miles never touch the real totals
    lock();
    TripTotals t = data.trip, k = data.tank;
    float odo = data.odoMiles;
    unlock();
    saveTrip("t", t);
    saveTrip("k", k);
    prefs.putFloat("odo", odo);
    lastSave = millis();
  }

  // Send one command and collect the reply up to the '>' prompt. The reply is
  // upper-cased with spaces removed; lines are separated by '\n'.
  bool cmd(const char *c, String &out, uint32_t timeoutMs = 1500) {
    char tmp[64];
    while (xStreamBufferReceive(link->rx, tmp, sizeof(tmp), 0) > 0) {}
    link->write(c);
    out = "";
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
      size_t n = xStreamBufferReceive(link->rx, tmp, sizeof(tmp), pdMS_TO_TICKS(20));
      for (size_t i = 0; i < n; i++) {
        char ch = toupper(tmp[i]);
        if (ch == '>') {
          out.replace("SEARCHING...", "");
          out.trim();
          return true;
        }
        if (ch == '\r' || ch == '\n') { if (out.length() && !out.endsWith("\n")) out += '\n'; }
        else if (ch != ' ' && ch != 0) out += ch;
      }
      if (!link->isOpen()) return false;
    }
    return false;
  }

  static int hexByte(const String &s, int i) { return strtol(s.substring(i, i + 2).c_str(), nullptr, 16); }

  // Mode 01 request; fills bytes[] with the data bytes of the first reply.
  bool pid(uint8_t p, uint8_t *bytes, int want) {
    char c[5];
    snprintf(c, sizeof(c), "01%02X", p);
    String r;
    if (!cmd(c, r)) return false;
    char hdr[5];
    snprintf(hdr, sizeof(hdr), "41%02X", p);
    int at = r.indexOf(hdr);
    if (at < 0 || (int)r.length() < at + 4 + want * 2) return false;
    for (int i = 0; i < want; i++) bytes[i] = hexByte(r, at + 4 + i * 2);
    return true;
  }

  bool isSupported(uint8_t p) const {
    if (p == 0) return true;
    int bank = (p - 1) / 32;
    if (bank > 2) return false;
    return supported[bank] & (1UL << (31 - (p - 1) % 32));
  }

  void setIncomplete(bool on) {
    lock();
    data.tankIncomplete = on;
    unlock();
    if (!data.simulated) prefs.putBool("kinc", on);
  }

  void loadCalInfo(ObdData &d) {
    d.fuelCal = fuelCal;
    d.calFills = prefs.getInt("calN", 0);
    d.lastFillEstimate = prefs.getFloat("lfEst", 0);
    d.lastFillPumped = prefs.getFloat("lfPump", 0);
    d.lastFill = (ObdData::FillResult)prefs.getUChar("lfWhy", ObdData::FILL_NONE);
  }

  // Unmonitored time: the display is running but can't find the adapter, so
  // the car may be driving untracked. (Adapter found but engine computer off
  // means the car is off - that's fine.)
  void watchForGaps(ObdData::State st) {
    bool suspicious = (st == ObdData::NOT_FOUND || st == ObdData::SEARCHING) && !onComputer && !data.simulated;
    if (!suspicious) { unmonitoredSince = 0; return; }
    if (!unmonitoredSince) unmonitoredSince = millis();
    if (millis() - unmonitoredSince > 5 * 60000UL && !data.tankIncomplete) {
      setIncomplete(true);
      events.add("No adapter for 5+ min: tank marked partly untracked");
    }
    if (millis() - unmonitoredSince > 5 * 60000UL && !odoGap) {
      odoGap = true;
      prefs.putBool("ogap", true);
    }
  }

  bool initAdapter() {
    String r;
    // A warm start resets everything like ATZ but skips the adapter's
    // self-test, about a second faster. Adapters without it get the full reset.
    if (!(cmd("ATWS", r, 2500) && r.indexOf("ELM") >= 0)) cmd("ATZ", r, 2500);
    vTaskDelay(pdMS_TO_TICKS(300));
    if (!cmd("ATE0", r)) return false;
    for (const char *c : {"ATL0", "ATS0", "ATH0", "ATAT1"}) cmd(c, r);
    // Start with the protocol this car answered on last time: searching
    // through all of them takes about 5 s on every connection. "A" keeps the
    // search as a fallback if the car doesn't answer on it.
    char sp[8];
    snprintf(sp, sizeof(sp), "ATSPA%c", proto);
    if (!proto || data.simulated || !cmd(sp, r) || r.indexOf("OK") < 0) cmd("ATSP0", r);
    return true;
  }

  // Note which protocol the adapter settled on ("A5": found by searching, 5).
  void rememberProtocol() {
    if (data.simulated) return;
    String r;
    if (!cmd("ATDPN", r) || !r.length()) return;
    char p = r[r.length() - 1];
    if (!isxdigit(p) || p == '0' || p == proto) return;
    proto = p;
    prefs.putUChar("proto", p);
    events.add("Car's protocol remembered (%c): quicker start next time", p);
  }

  // Ask the ECU which PIDs it supports; fails while the ignition is off.
  bool probeEcu() {
    uint8_t b[4];
    // The first request makes the adapter search for the protocol (slow).
    String r;
    if (!cmd("0100", r, 15000)) return false;
    int at = r.indexOf("4100");
    if (at < 0 || (int)r.length() < at + 12) return false;
    supported[0] = strtoul(r.substring(at + 4, at + 12).c_str(), nullptr, 16);
    supported[1] = supported[2] = 0;
    if (isSupported(0x20) && pid(0x20, b, 4)) supported[1] = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
    lock();
    data.useMaf = isSupported(0x10);
    unlock();
    fails = 0;
    lastMil = 0;
    lastSlow = 0;
    rememberProtocol();
    lastFast = millis();
    return true;
  }

  // Fuel trims, timing and throttle - whichever this ECU supports.
  void readDetails() {
    uint8_t b[2];
    float st[2] = {NAN, NAN}, lt[2] = {NAN, NAN}, timing = NAN, thr = NAN;
    auto trim = [](uint8_t v) { return (v - 128) * 100.0f / 128.0f; };
    for (int bank = 0; bank < 2; bank++) {
      if (isSupported(0x06 + bank * 2) && pid(0x06 + bank * 2, b, 1)) st[bank] = trim(b[0]);
      if (isSupported(0x07 + bank * 2) && pid(0x07 + bank * 2, b, 1)) lt[bank] = trim(b[0]);
    }
    if (isSupported(0x0E) && pid(0x0E, b, 1)) timing = b[0] / 2.0f - 64;
    if (isSupported(0x11) && pid(0x11, b, 1)) thr = b[0] * 100 / 255.0f;
    lock();
    for (int i = 0; i < 2; i++) data.stft[i] = st[i], data.ltft[i] = lt[i];
    data.timingDeg = timing;
    data.throttlePct = thr;
    float mph = data.mph, rpm = data.rpm;
    unlock();
    if (health) health->details(st, lt, timing, thr, mph, rpm);
  }

  void readCodes() {
    String r;
    if (!cmd("03", r, 4000)) return;
    uint16_t found[8];
    int n = 0;
    int start = 0;
    while (start < (int)r.length()) {
      int end = r.indexOf('\n', start);
      if (end < 0) end = r.length();
      String line = r.substring(start, end);
      int at = line.indexOf("43");
      if (at >= 0) {
        String body = line.substring(at + 2);
        // CAN replies carry a count byte first (odd number of bytes).
        int off = (body.length() / 2) % 2 ? 2 : 0;
        for (int i = off; i + 4 <= (int)body.length() && n < 8; i += 4) {
          uint16_t c = strtoul(body.substring(i, i + 4).c_str(), nullptr, 16);
          if (c) found[n++] = c;
        }
      }
      start = end + 1;
    }
    lock();
    bool changed = !data.dtcsRead || n != data.nDtcs || memcmp(data.dtcs, found, sizeof(uint16_t) * n);
    data.nDtcs = n;
    memcpy(data.dtcs, found, sizeof(uint16_t) * n);
    data.dtcsRead = true;
    unlock();
    if (changed && !data.simulated) {
      String list;
      for (int i = 0; i < n; i++) list += " " + ObdData::dtcText(found[i]);
      events.add("Trouble codes:%s", n ? list.c_str() : " none");
    }
  }

  // ---------------------------------------------------------- transmission
  // The transmission computer keeps its own trouble codes; the engine computer
  // only passes on P0700 ("the transmission has something stored"). It isn't
  // part of standard OBD-II, so it is asked directly: KWP2000 on the same
  // K-line, at its own address, with "read trouble codes by status" (service
  // 0x18). Nothing is ever cleared or written there.
  //
  // Its address on this car isn't documented, so the first check tries the
  // range the standard gives transmissions (0x18-0x1F) and remembers whichever
  // answers. Every raw reply goes to the event log, so a car that answers
  // differently can be sorted out from the log.

  static String oneLine(String s) {
    s.replace("\n", " | ");
    return s.length() > 110 ? s.substring(0, 110) + "..." : s;
  }

  // Which computers answer a standard request. With headers on a reply reads
  // "83 F1 11 41 00 ...": the third byte is the sender.
  String whoAnswers(uint8_t *addrs, int &n, int max) {
    String r, list;
    n = 0;
    cmd("ATH1", r);
    if (cmd("0100", r, 6000)) {
      int start = 0;
      while (start < (int)r.length()) {
        int end = r.indexOf('\n', start);
        if (end < 0) end = r.length();
        String line = r.substring(start, end);
        int at = line.indexOf("4100");
        if ((at == 6 || at == 8) && n < max) {
          addrs[n++] = hexByte(line, 4);
          list += " " + line.substring(4, 6);
        }
        start = end + 1;
      }
    }
    cmd("ATH0", r);
    return list.length() ? list : String(" none");
  }

  // Wake the computer at `addr` and open a session with it.
  bool wakeModule(uint8_t addr, bool slowInit, String &r) {
    char c[20];
    cmd("ATPC", r);  // end the session with the engine computer
    cmd(slowInit ? "ATSP4" : "ATSP5", r);
    if (slowInit) {
      snprintf(c, sizeof(c), "ATIIA%02X", addr);
      cmd(c, r);
    }
    snprintf(c, sizeof(c), "ATSH81%02XF1", addr);  // to addr, from the tester (F1)
    cmd(c, r);
    snprintf(c, sizeof(c), "ATWM81%02XF13E", addr);  // keep-alive goes there too
    cmd(c, r);
    if (!cmd(slowInit ? "ATSI" : "ATFI", r, 8000)) return false;
    // "BUS INIT: OK". An adapter without this command ("?") wakes the
    // computer with the first request instead.
    return r.indexOf("ERROR") < 0;
  }

  // Ask an awake computer for its codes. 1 = listed, 0 = no reply, -1 = refused.
  int askModuleCodes(uint16_t *codes, bool *now, int &n, String &raw) {
    n = 0;
    raw = "";
    for (int attempt = 0; attempt < 3; attempt++) {
      String r;
      if (attempt == 1) cmd("1081", r, 3000);  // some want a diagnostic session opened first
      const char *ask = attempt == 2 ? "1802FF00" : "1800FF00";  // two common forms of the request
      bool got = cmd(ask, r, 6000);
      raw += String(raw.length() ? " | " : "") + (attempt == 1 ? "1081+" : "") + ask + ": " + (got ? r : String("(timeout)"));
      if (!got) return 0;
      bool refused = false;
      int start = 0;
      while (start < (int)r.length()) {
        int end = r.indexOf('\n', start);
        if (end < 0) end = r.length();
        String line = r.substring(start, end);
        start = end + 1;
        if (line.startsWith("7F18")) {
          if (hexByte(line, 4) != 0x78) refused = true;  // 0x78: "wait, working on it"
          continue;
        }
        if (!line.startsWith("58") || line.length() < 4) continue;
        // 58 <count> then <code high> <code low> <status> for each.
        int count = hexByte(line, 2);
        for (int i = 0; i < count && n < 8 && 4 + i * 6 + 6 <= (int)line.length(); i++) {
          uint16_t c = strtoul(line.substring(4 + i * 6, 8 + i * 6).c_str(), nullptr, 16);
          if (!c) continue;
          codes[n] = c;
          now[n++] = (hexByte(line, 8 + i * 6) & 0x60) == 0x60;  // status bits 5-6: present right now
        }
        return 1;
      }
      if (!refused) return 0;
    }
    return -1;
  }

  void checkTransmission() {
    lock();
    bool moving = !isnan(data.mph) && data.mph > 1;
    // Not starting never wipes what an earlier check found.
    if (!moving || data.trans != ObdData::TR_DONE) data.trans = moving ? ObdData::TR_MOVING : ObdData::TR_READING;
    unlock();
    if (moving) return;
    bool quiet = data.simulated;  // the demo car stays out of the event log and settings
    saveTrips();

    String r;
    bool slowInit = cmd("ATDPN", r) && r.endsWith("4");  // "A4": KWP2000 with the slow (5-baud) start
    String proto = r;
    uint8_t seen[4];
    int nSeen = 0;
    String who = whoAnswers(seen, nSeen, 4);
    if (!quiet) events.add("Transmission check: adapter protocol %s, standard requests answered by%s", proto.c_str(), who.c_str());

    // Remembered address first, then any transmission-range computer that
    // answered above, then the rest of the range.
    uint8_t order[12];
    int nOrder = 0;
    auto consider = [&](uint8_t a) {
      for (int i = 0; i < nOrder; i++)
        if (order[i] == a) return;
      if (a && nOrder < 12) order[nOrder++] = a;
    };
    if (!quiet) consider(prefs.getUChar("tcm", 0));
    for (int i = 0; i < nSeen; i++)
      if (seen[i] >= 0x18 && seen[i] <= 0x1F) consider(seen[i]);
    for (uint8_t a = 0x18; a <= 0x1F; a++) consider(a);

    ObdData::TransState result = ObdData::TR_NO_ANSWER;
    uint16_t codes[8];
    bool now[8];
    int n = 0;
    uint8_t found = 0;
    String silent, lastWake;
    for (int i = 0; i < nOrder && result != ObdData::TR_DONE && link->isOpen(); i++) {
      char hex[4];
      snprintf(hex, sizeof(hex), " %02X", order[i]);
      if (!wakeModule(order[i], slowInit, r)) {
        silent += hex;
        lastWake = r;
        continue;
      }
      String raw;
      int got = askModuleCodes(codes, now, n, raw);
      if (!quiet) events.add("Transmission at%s: %s", hex, oneLine(raw).c_str());
      if (got == 0) { silent += hex; continue; }
      found = order[i];
      result = got > 0 ? ObdData::TR_DONE : ObdData::TR_REFUSED;
      cmd("82", r, 2000);  // end the session politely
    }

    if (!quiet) {
      if (silent.length()) events.add("Transmission check: no answer at%s (%s)", silent.c_str(), oneLine(lastWake).c_str());
      if (result == ObdData::TR_DONE) {
        String list;
        for (int i = 0; i < n; i++) list += " " + ObdData::dtcText(codes[i]) + (now[i] ? "(now)" : "");
        events.add("Transmission codes:%s", n ? list.c_str() : " none");
        if (found != prefs.getUChar("tcm", 0)) prefs.putUChar("tcm", found);
      } else if (result == ObdData::TR_REFUSED) {
        events.add("Transmission computer answered at %02X but wouldn't list its codes", found);
      }
    }
    lock();
    data.trans = result;
    data.transAddr = found;
    data.nTransDtcs = result == ObdData::TR_DONE ? n : 0;
    memcpy(data.transDtcs, codes, sizeof(uint16_t) * data.nTransDtcs);
    memcpy(data.transNow, now, sizeof(bool) * data.nTransDtcs);
    unlock();

    // Back to the engine computer: reset the adapter and find it again.
    if (!(initAdapter() && probeEcu())) {
      setState(ObdData::NO_ECU);
      endDrive();
    }
  }

  void handleRequests() {
    if (reqTrans && data.state != ObdData::LIVE) {
      reqTrans = false;
      lock();
      if (data.trans != ObdData::TR_DONE) data.trans = ObdData::TR_CAR_OFF;
      unlock();
    }
    if (reqResetTrip) {
      reqResetTrip = false;
      events.add("Trip meter reset (was %.1f mi)", data.trip.miles);
      lock();
      data.trip = TripTotals();
      unlock();
      saveTrips();
    }
    if (reqFillUp > 0) {
      float pumped = reqFillUp;
      reqFillUp = 0;
      lock();
      float used = data.tank.gallons;
      float tankMiles = data.tank.miles;
      bool wasFull = data.tankFromFull && data.tankStartUsed == 0;  // known-full start, not a gauge guess
      bool fromFull = wasFull && !data.tankIncomplete;
      data.tank = TripTotals();
      data.tankFromFull = true;
      data.tankStartUsed = 0;
      data.tankIncomplete = false;
      unlock();
      if (!data.simulated) prefs.putFloat("kstart", 0);
      setIncomplete(false);
      // Calibrate against the pump only when this tank was tracked from a
      // full fill-up (the first fill-up after install just starts tracking),
      // move halfway toward the pump's answer, and ignore wild results from
      // missed drives or partial fills.
      float ratio = used > 0 ? pumped / used : 0;
      ObdData::FillResult why;
      if (!wasFull) why = ObdData::FILL_STARTED;
      else if (!fromFull) why = ObdData::FILL_UNTRACKED;
      else if (used <= 3 || pumped <= 3) why = ObdData::FILL_TOO_SMALL;
      else if (ratio <= 0.6f || ratio >= 1.6f) why = ObdData::FILL_TOO_DIFFERENT;
      else why = ObdData::FILL_CALIBRATED;
      if (!data.simulated) {
        if (why == ObdData::FILL_CALIBRATED) {
          fuelCal = constrain(fuelCal * (1 + 0.5f * (ratio - 1)), 0.5f, 2.0f);
          prefs.putFloat("cal", fuelCal);
          prefs.putInt("calN", prefs.getInt("calN", 0) + 1);
        }
        prefs.putFloat("lfEst", used);
        prefs.putFloat("lfPump", pumped);
        prefs.putUChar("lfWhy", why);
        prefs.putBool("kfull", true);
      }
      static const char *WHY[] = {"", "calibrated", "started tracking", "not calibrated: untracked driving",
                                  "not calibrated: too different (partial fill?)", "not calibrated: too few gallons"};
      events.add("Fill-up: pump %.1f gal, display counted %.1f gal -> %s%s", pumped, used, WHY[why],
                 data.simulated ? " (demo)" : "");
      if (hist) hist->addFill(data.odoMiles, pumped, used, tankMiles, fromFull);
      lock();
      if (data.simulated) {  // the demo shows the result without saving it
        data.lastFillEstimate = used, data.lastFillPumped = pumped, data.lastFill = why;
      } else {
        loadCalInfo(data);
      }
      unlock();
      saveTrips();
    }
    if (reqTankComplete) {
      reqTankComplete = false;
      setIncomplete(false);
      events.add("Tank marked as tracked");
    }
    if (reqLevel > 0) {
      float pct = reqLevel - 1;
      reqLevel = 0;
      events.add("Fuel level set to %.0f%% from the gauge", pct);
      lock();
      data.tank = TripTotals();
      data.tankStartUsed = data.tankCapacity * (1 - pct / 100);
      data.tankFromFull = true;
      data.tankIncomplete = false;
      float start = data.tankStartUsed;
      unlock();
      if (!data.simulated) {
        prefs.putFloat("kstart", start);
        prefs.putBool("kfull", true);
        prefs.putBool("kinc", false);
      }
      saveTrips();
    }
    if (reqTankFull) {
      reqTankFull = false;
      lock();
      data.tankFromFull = true;
      unlock();
      if (!data.simulated) prefs.putBool("kfull", true);
    }
    if (reqCalReset) {
      reqCalReset = false;
      events.add("MPG calibration reset (was x%.2f)", (float)fuelCal);
      if (!data.simulated) {
        prefs.putInt("calN", 0);
        prefs.remove("lfWhy");
      }
      reqCal = 1.0f;  // re-scales this tank's gallons back to the default below
    }
    if (reqCal > 0) {
      float c = reqCal;
      reqCal = 0;
      float k = c / fuelCal;
      lock();
      float before = data.tank.gallons;
      data.tank.gallons *= k;
      data.trip.gallons = max(0.0f, data.trip.gallons + before * (k - 1));
      unlock();
      fuelCal = c;
      if (!data.simulated) prefs.putFloat("cal", c);
      lock();
      loadCalInfo(data);
      unlock();
      saveTrips();
    }
  }

  // One round of polling. Fast values every round, slow ones every few seconds.
  void poll() {
    uint8_t b[4];
    uint32_t now = millis();
    bool ok = false;

    float mph = NAN, rpm = NAN, maf = NAN, mafMeasured = NAN;
    if (pid(0x0D, b, 1)) { mph = b[0] * 0.621371f * distCal; ok = true; }
    if (pid(0x0C, b, 2)) { rpm = (b[0] * 256 + b[1]) / 4.0f; ok = true; }
    if (data.useMaf) {
      if (pid(0x10, b, 2)) maf = (b[0] * 256 + b[1]) / 100.0f;
      mafMeasured = maf;
    } else if (isSupported(0x0B) && pid(0x0B, b, 1)) {
      mapKpa = b[0];
    }

    float coolant = NAN, intake = NAN, load = NAN, volts = NAN;
    bool slow = now - lastSlow > 2000;
    if (slow) {
      lastSlow = now;
      // Unsupported requests just burn a timeout on the slow K-line.
      if (isSupported(0x05) && pid(0x05, b, 1)) coolant = (b[0] - 40) * 1.8f + 32;
      if (isSupported(0x0F) && pid(0x0F, b, 1)) intake = (b[0] - 40) * 1.8f + 32;
      if (isSupported(0x04) && pid(0x04, b, 1)) load = b[0] * 100 / 255.0f;
      String r;
      if (cmd("ATRV", r)) volts = r.toFloat();
    }
    // Details every 10 s (2 s on a details screen), and sooner under hard
    // acceleration so the health checks see fuel trims under load.
    bool loaded = !isnan(data.loadPct) && data.loadPct >= 65 && !isnan(mph) && mph > 10;
    if (now - lastDetail > (detailFocus ? 2000UL : 10000UL) || (loaded && now - lastDetail > 2500)) {
      lastDetail = now;
      readDetails();
    }

    if (reqClear && !readOnly) {
      reqClear = false;
      String r;
      cmd("04", r, 4000);
      if (!data.simulated) events.add("Trouble codes cleared");
      lock();
      data.milOn = false;
      data.nDtcs = 0;
      unlock();
      lastMil = 0;
    }
    if (now - lastMil > 15000 || lastMil == 0) {
      lastMil = now;
      if (pid(0x01, b, 1)) {
        bool mil = b[0] & 0x80;
        int count = b[0] & 0x7F;
        if (mil != data.milOn && !data.simulated) events.add("Check engine light %s", mil ? "ON" : "off");
        lock();
        data.milOn = mil;
        bool need = count > 0 && (!data.dtcsRead || count != data.nDtcs);
        if (count == 0) { data.nDtcs = 0; data.dtcsRead = true; }
        unlock();
        if (need) readCodes();
      }
    }

    if (!ok) {
      if (++fails >= 3) {
        setState(ObdData::NO_ECU);
        saveTrips();
        endDrive();
      }
      return;
    }
    fails = 0;

    // Fuel flow (gal/h). Speed-density estimate when there is no MAF sensor.
    float intakeK = (isnan(data.intakeF) ? 77 : data.intakeF - 32) / 1.8f + 273.15f;
    if (isnan(maf) && !isnan(mapKpa) && !isnan(rpm))
      maf = rpm * mapKpa / intakeK / 120.0f * 0.80f * displacement * 28.97f / 8.314f;
    float gph = isnan(maf) ? NAN : maf * 3600.0f / (14.7f * 2802.0f) * fuelCal;

    float dt = min(2.0f, (now - lastFast) / 1000.0f);
    lastFast = now;
    float k = min(1.0f, dt / 2.0f);  // ~2 s smoothing
    if (!isnan(mph)) mphEma = isnan(mphEma) ? mph : mphEma + (mph - mphEma) * k;
    if (!isnan(gph)) gphEma = isnan(gphEma) ? gph : gphEma + (gph - gphEma) * k;

    lock();
    data.mph = mph;
    data.rpm = rpm;
    data.gph = gphEma;
    data.instMpg = (!isnan(mphEma) && mphEma > 2 && gphEma > 0.01f) ? min(99.0f, mphEma / gphEma) : NAN;
    if (!isnan(coolant)) data.coolantF = coolant;
    if (!isnan(intake)) data.intakeF = intake;
    if (!isnan(load)) data.loadPct = load;
    if (!isnan(volts) && volts > 5) data.volts = volts;
    if (!isnan(mph)) {
      float mi = mph * dt / 3600.0f;
      data.trip.miles += mi;
      data.drive.miles += mi;
      data.tank.miles += mi;
      data.odoMiles += mi;
    }
    if (!isnan(gph)) {
      float gal = gph * dt / 3600.0f;
      data.trip.gallons += gal;
      data.drive.gallons += gal;
      data.tank.gallons += gal;
    }
    if (!isnan(rpm) && rpm > 300) {
      data.trip.seconds += dt;
      data.drive.seconds += dt;
      data.tank.seconds += dt;
    }
    data.updatedAt = now;
    unlock();
    lastLive = now;

    if (hist) hist->tick(dt, mph, gph);
    if (health) {
      health->fast(dt, mph, rpm, mafMeasured, gph);
      if (slow) health->slow(coolant, intake, load, volts);
    }
    if (now - lastSave > 60000) saveTrips();
  }

  // Sleep, but keep serving requests and notice a switch to/from the demo car.
  void idle(uint32_t ms) {
    bool sim = simulate;
    for (uint32_t t = 0; t < ms && sim == simulate && !reqReconnect; t += 100) {
      handleRequests();
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  void run() {
    if (hist) hist->begin();
    for (;;) {
      handleRequests();
      ElmLink *want = simulate ? (ElmLink *)&sim : (ElmLink *)&ble;
      if (want != link || reqReconnect) {
        reqReconnect = false;
        if (link) link->close();
        link = want;
        endDrive();
        if (hist) hist->setDemo(simulate);
        if (health) health->setDemo(simulate);
        // Demo car gets its own throwaway trip; the real one comes back after.
        TripTotals t = simulate ? TripTotals() : loadTrip("t");
        TripTotals k = simulate ? TripTotals() : loadTrip("k");
        lock();
        ObdData fresh;
        fresh.tankCapacity = data.tankCapacity;
        fresh.trip = t;
        fresh.tank = k;
        fresh.odoMiles = prefs.getFloat("odo", 0);
        fresh.tankFromFull = simulate || prefs.getBool("kfull", false);
        fresh.tankStartUsed = simulate ? 0 : prefs.getFloat("kstart", 0);
        fresh.tankIncomplete = !simulate && prefs.getBool("kinc", false);
        unmonitoredSince = 0;
        loadCalInfo(fresh);
        fresh.simulated = simulate;
        data = fresh;
        unlock();
      }

      if (!link->isOpen()) {
        endDrive();
        setState(ObdData::SEARCHING);
        char name[32] = "";
        if (!link->open(name, sizeof(name))) {
          setState(ObdData::NOT_FOUND);
          watchForGaps(ObdData::NOT_FOUND);
          idle(6000);
          continue;
        }
        lock();
        strlcpy(data.adapter, name, sizeof(data.adapter));
        data.state = ObdData::INITIALIZING;
        unlock();
        if (!link->address().isEmpty() && link->address() != ble.preferred) {
          ble.preferred = link->address();
          prefs.putString("addr", ble.preferred);
        }
        if (!initAdapter()) {
          if (!data.simulated) events.add("Adapter %s found but didn't answer setup", data.adapter);
          link->close();
          idle(3000);
          continue;
        }
        setState(ObdData::NO_ECU);
      }

      if (data.state != ObdData::LIVE) {
        // Battery voltage with the engine off (the adapter answers this itself).
        String rv;
        if (health && cmd("ATRV", rv)) health->engineOff(rv.toFloat());
        if (probeEcu()) {
          // A new drive once the engine has been off a couple of minutes (a
          // brief connection hiccup mid-drive doesn't start one).
          if (!lastLive || millis() - lastLive > 120000) {
            lock();
            data.drive = TripTotals();
            unlock();
          }
          setState(ObdData::LIVE);
          if (hist) hist->beginDrive(data.odoMiles);
          if (health) health->beginDrive(data.useMaf);
        } else {
          setState(ObdData::NO_ECU);
          watchForGaps(ObdData::NO_ECU);  // adapter answers, engine off: not driving
          idle(4000);
        }
        continue;
      }

      watchForGaps(data.state);
      if (reqTrans) {
        reqTrans = false;
        checkTransmission();
        continue;
      }
      poll();
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
};
