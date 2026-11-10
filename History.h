// Driving history for the phone page's charts: one record per drive, one per
// fill-up, and lifetime efficiency by speed band. Stored as small binary files
// on the flash's FAT partition (plenty of room: ~10 MB).
//
// Fed from the OBD task (tick/beginDrive/endDrive/addFill), read by the web
// server (json). A mutex keeps the two apart. The display has no clock, so
// drives are identified by number and odometer rather than date.
#pragma once
#include <Arduino.h>
#include <FFat.h>
#include <vector>
#include "EventLog.h"

struct DriveRec {
  uint32_t epoch;  // reserved (no clock; always 0)
  float startOdo;  // tracked miles at start
  float miles, gallons;
  uint32_t seconds, idleSeconds;
  float idleGallons;
  uint8_t maxMph, pad[3];
};

struct FillRec {
  uint32_t epoch;
  float odo;        // tracked miles
  float pumped;     // gallons from the pump
  float estimated;  // gallons the display had counted for this tank
  float tankMiles;
  uint8_t valid, pad[3];  // tank tracked from full and complete -> real pump MPG
};

class History {
public:
  // Speed bands for "efficiency by speed" (mph lower bounds; idle is separate).
  static constexpr int BANDS = 8;
  static constexpr uint8_t BAND_MIN[BANDS] = {1, 15, 25, 35, 45, 55, 65, 75};

  void begin() {
    mtx = xSemaphoreCreateMutex();
    // Formats the partition on first use (takes a few seconds, once).
    ok = FFat.begin(true);
    if (ok) load();
  }

  // Demo car: use separate scratch files (wiped each time demo starts) so demo
  // data never lands in the real history.
  void setDemo(bool on) {
    lock();
    demo = on;
    clearRam();
    if (ok) {
      if (on) removeFiles();
      else loadLocked();
    }
    unlock();
  }

  // No clock on board: the epoch fields are kept in the file format but stay 0.
  uint32_t now() const { return 0; }

  void beginDrive(float odo) {
    lock();
    cur = DriveRec{now(), odo, 0, 0, 0, 0, 0, 0, {0, 0, 0}};
    curMs = millis();
    curIndex = -1;
    driving = true;
    unlock();
  }

  void endDrive() {
    lock();
    if (driving && curIndex >= 0) {
      saveDrive();
      if (!demo)
        events.add("Drive ended: %.1f mi, %.1f MPG, %lu min, top %u mph", cur.miles,
                   cur.gallons > 0.01f ? cur.miles / cur.gallons : 0.0f, (unsigned long)cur.seconds / 60, cur.maxMph);
    }
    driving = false;
    curIndex = -1;
    unlock();
  }

  // Called every OBD polling round with the time since the previous one.
  void tick(float dt, float mph, float gph) {
    if (!driving || isnan(mph)) return;
    lock();
    float mi = mph * dt / 3600.0f;
    float gal = isnan(gph) ? 0 : gph * dt / 3600.0f;
    cur.miles += mi;
    cur.gallons += gal;
    cur.seconds = (millis() - curMs) / 1000;
    if (mph > cur.maxMph) cur.maxMph = min(255.0f, mph);
    if (mph < 1) {
      cur.idleSeconds += dt;
      cur.idleGallons += gal;
      idleSec += dt;
      idleGal += gal;
    } else {
      int b = BANDS - 1;
      while (b > 0 && mph < BAND_MIN[b]) b--;
      bandMiles[b] += mi;
      bandGal[b] += gal;
      bandSec[b] += dt;
    }
    // Only drives that actually went somewhere are recorded.
    if (curIndex < 0 && cur.miles >= 0.3f) {
      drives.push_back(cur);
      curIndex = drives.size() - 1;
      saveDrive();
    } else if (curIndex >= 0 && millis() - lastSave > 30000) {
      saveDrive();  // power can vanish with the ignition: keep it fresh
    }
    bool bandsDue = millis() - lastBandSave > 60000;
    unlock();
    if (bandsDue) saveBands();
  }

  void addFill(float odo, float pumped, float estimated, float tankMiles, bool valid) {
    lock();
    FillRec f{now(), odo, pumped, estimated, tankMiles, (uint8_t)valid, {0, 0, 0}};
    fills.push_back(f);
    if (ok) append(path("fills"), &f, sizeof(f));
    unlock();
  }

  // Raw copy of a real (not demo) history file, for backups.
  std::vector<uint8_t> rawFile(const char *name) {
    std::vector<uint8_t> out;
    lock();
    File f = FFat.open(String("/") + name + ".bin", FILE_READ);
    if (f) {
      out.resize(f.size());
      out.resize(f.read(out.data(), out.size()));
      f.close();
    }
    unlock();
    return out;
  }

  void clear() {
    lock();
    clearRam();
    if (ok) removeFiles();
    unlock();
    events.add("Drive history cleared");
  }

  // Compact JSON for the charts: arrays of numbers, newest last.
  String json(size_t maxDrives = 400) {
    lock();
    String o;
    o.reserve(8192 + drives.size() * 48);
    o += "{\"now\":" + String(now()) + ",\"storage\":" + String(ok ? "true" : "false") + ",\"drives\":[";
    size_t start = drives.size() > maxDrives ? drives.size() - maxDrives : 0;
    for (size_t i = start; i < drives.size(); i++) {
      const DriveRec &d = drives[i];
      if (i > start) o += ',';
      o += '[' + String(d.epoch) + ',' + String(d.startOdo, 1) + ',' + String(d.miles, 2) + ',' + String(d.gallons, 3) +
           ',' + String(d.seconds) + ',' + String(d.idleSeconds) + ',' + String(d.idleGallons, 3) + ',' +
           String(d.maxMph) + ']';
    }
    o += "],\"fills\":[";
    for (size_t i = 0; i < fills.size(); i++) {
      const FillRec &f = fills[i];
      if (i) o += ',';
      o += '[' + String(f.epoch) + ',' + String(f.odo, 1) + ',' + String(f.pumped, 2) + ',' + String(f.estimated, 2) +
           ',' + String(f.tankMiles, 1) + ',' + String(f.valid) + ']';
    }
    o += "],\"bands\":{\"min\":[";
    for (int b = 0; b < BANDS; b++) o += (b ? "," : "") + String(BAND_MIN[b]);
    o += "],\"mi\":[";
    for (int b = 0; b < BANDS; b++) o += (b ? "," : "") + String(bandMiles[b], 2);
    o += "],\"gal\":[";
    for (int b = 0; b < BANDS; b++) o += (b ? "," : "") + String(bandGal[b], 3);
    o += "],\"sec\":[";
    for (int b = 0; b < BANDS; b++) o += (b ? "," : "") + String(bandSec[b], 0);
    o += "],\"idleSec\":" + String(idleSec, 0) + ",\"idleGal\":" + String(idleGal, 3) + "}}";
    unlock();
    return o;
  }

  // Periodic save of the speed-band totals (called from the OBD task).
  void saveBands() {
    lock();
    lastBandSave = millis();
    if (ok) {
      File f = FFat.open(path("speed"), FILE_WRITE);
      if (f) {
        f.write((uint8_t *)bandMiles, sizeof(bandMiles));
        f.write((uint8_t *)bandGal, sizeof(bandGal));
        f.write((uint8_t *)bandSec, sizeof(bandSec));
        f.write((uint8_t *)&idleSec, sizeof(idleSec));
        f.write((uint8_t *)&idleGal, sizeof(idleGal));
        f.close();
      }
    }
    unlock();
  }

  // Demo mode only: fill the scratch history with plausible drives so the
  // charts can be tried out (development).
  bool fillDemo() {
    lock();
    if (!demo) { unlock(); return false; }
    clearRam();
    float odo = 151;
    for (int i = 0; i < 20; i++) {
      bool hwy = i % 3 == 0;
      float mi = hwy ? 18 + (i * 7) % 20 : 2 + (i * 5) % 7;
      float mpg = (hwy ? 19.5f : 14.5f) + (i % 4) * 0.6f + i * 0.08f;
      DriveRec d{0, odo, mi, mi / mpg, (uint32_t)(mi / (hwy ? 55 : 24) * 3600), 120, 0.02f, (uint8_t)(hwy ? 72 : 44), {0, 0, 0}};
      drives.push_back(d);
      odo += mi;
    }
    const float mi[BANDS] = {22, 64, 120, 140, 110, 160, 230, 14}, gal[BANDS] = {2.3, 5.1, 8.6, 8.9, 6.1, 7.9, 11.8, 0.8};
    for (int b = 0; b < BANDS; b++) bandMiles[b] = mi[b], bandGal[b] = gal[b], bandSec[b] = mi[b] * 90;
    idleSec = 30000, idleGal = 4.8f;
    unlock();
    return true;
  }

  // ---- read-only views for the display's History screens
  struct BandStats {
    float miles[BANDS], gallons[BANDS], seconds[BANDS];
    float idleSec, idleGal;
  };
  BandStats bandStats() {
    lock();
    BandStats b;
    memcpy(b.miles, bandMiles, sizeof(bandMiles));
    memcpy(b.gallons, bandGal, sizeof(bandGal));
    memcpy(b.seconds, bandSec, sizeof(bandSec));
    b.idleSec = idleSec;
    b.idleGal = idleGal;
    unlock();
    return b;
  }

  // The last `n` drives, oldest first.
  std::vector<DriveRec> recentDrives(size_t n) {
    lock();
    size_t from = drives.size() > n ? drives.size() - n : 0;
    std::vector<DriveRec> v(drives.begin() + from, drives.end());
    unlock();
    return v;
  }

  struct Totals {
    size_t drives = 0;
    float miles = 0, gallons = 0;
  };
  Totals totals() {
    lock();
    Totals t;
    t.drives = drives.size();
    for (auto &d : drives) t.miles += d.miles, t.gallons += d.gallons;
    unlock();
    return t;
  }

  // Re-read everything from the files (testing: proves what was saved).
  void reload() {
    lock();
    clearRam();
    if (ok) loadLocked();
    unlock();
  }

  size_t driveCount() {
    lock();
    size_t n = drives.size();
    unlock();
    return n;
  }

private:
  SemaphoreHandle_t mtx = nullptr;
  bool ok = false, demo = false, driving = false;
  std::vector<DriveRec> drives;
  std::vector<FillRec> fills;
  DriveRec cur{};
  int curIndex = -1;
  uint32_t curMs = 0, lastSave = 0, lastBandSave = 0;
  float bandMiles[BANDS] = {}, bandGal[BANDS] = {}, bandSec[BANDS] = {};
  float idleSec = 0, idleGal = 0;

  void lock() { if (mtx) xSemaphoreTake(mtx, portMAX_DELAY); }
  void unlock() { if (mtx) xSemaphoreGive(mtx); }

  String path(const char *name) const { return String(demo ? "/demo_" : "/") + name + ".bin"; }

  void removeFiles() {
    for (const char *n : {"drives", "fills", "speed"}) FFat.remove(path(n));
  }

  void clearRam() {
    drives.clear();
    fills.clear();
    memset(bandMiles, 0, sizeof(bandMiles));
    memset(bandGal, 0, sizeof(bandGal));
    memset(bandSec, 0, sizeof(bandSec));
    idleSec = idleGal = 0;
    driving = false;
    curIndex = -1;
  }

  void load() {
    lock();
    loadLocked();
    unlock();
  }

  void loadLocked() {
    readAll(path("drives"), drives);
    readAll(path("fills"), fills);
    File f = FFat.open(path("speed"), FILE_READ);
    if (f && f.size() == sizeof(bandMiles) * 3 + sizeof(float) * 2) {
      f.read((uint8_t *)bandMiles, sizeof(bandMiles));
      f.read((uint8_t *)bandGal, sizeof(bandGal));
      f.read((uint8_t *)bandSec, sizeof(bandSec));
      f.read((uint8_t *)&idleSec, sizeof(idleSec));
      f.read((uint8_t *)&idleGal, sizeof(idleGal));
    }
    if (f) f.close();
  }

  template <typename T>
  void readAll(const String &file, std::vector<T> &out) {
    out.clear();
    File f = FFat.open(file, FILE_READ);
    if (!f) return;
    T rec;
    while (f.read((uint8_t *)&rec, sizeof(T)) == sizeof(T)) out.push_back(rec);
    f.close();
  }

  template <typename T>
  void rewrite(const String &file, const std::vector<T> &v) {
    if (!ok) return;
    File f = FFat.open(file, FILE_WRITE);
    if (!f) return;
    for (auto &r : v) f.write((const uint8_t *)&r, sizeof(T));
    f.close();
  }

  void append(const String &file, const void *rec, size_t n) {
    File f = FFat.open(file, FILE_APPEND);
    if (!f) return;
    f.write((const uint8_t *)rec, n);
    f.close();
  }

  // Write the in-progress drive into its slot (it is always the last record).
  void saveDrive() {
    lastSave = millis();
    if (curIndex < 0) return;
    drives[curIndex] = cur;
    if (!ok) return;
    File f = FFat.open(path("drives"), "r+");
    if (!f) f = FFat.open(path("drives"), FILE_WRITE);
    if (!f) return;
    f.seek(curIndex * sizeof(DriveRec));
    f.write((const uint8_t *)&drives[curIndex], sizeof(DriveRec));
    f.close();
  }
};

constexpr uint8_t History::BAND_MIN[History::BANDS];
