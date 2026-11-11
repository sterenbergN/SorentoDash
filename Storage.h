// The flash file store for drive history, health state, the event log and
// staged restores.
//
// In the car the display loses power without warning: when the key is turned
// off, and about a second after every power-up while the starter cranks. The
// old FAT file system couldn't take that - it rewrites its file table and
// directory in place (erase, then write), so a cut between the two wiped every
// file at once (it happened five times in six weeks before it was noticed).
// LittleFS never overwrites live data: a power cut leaves either the old or the
// new version of a file, nothing in between.
//
// Every write here is checked by reading it back, so a store that mounts but
// can't save is noticed (healthy()) instead of silently dropping data.
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <vector>

namespace Storage {

// The partition keeps its old "ffat" name: the partition table can't be changed
// by a Wi-Fi update.
constexpr const char *PARTITION = "ffat";
constexpr const char *BASE = "/littlefs";
// Never format before the power has been steady this long (engine cranking can
// make a good file system look unreadable for a moment).
constexpr uint32_t SETTLE_MS = 6000;

inline bool mounted = false, formatted = false;
inline volatile uint32_t writes = 0, failures = 0, failStreak = 0;

inline bool note(bool ok) {
  writes = writes + 1;
  if (ok) failStreak = 0;
  else failures = failures + 1, failStreak = failStreak + 1;
  return ok;
}

// Mounted and the latest writes were read back correctly.
inline bool healthy() { return mounted && failStreak < 3; }

inline bool mount() { return LittleFS.begin(false, BASE, 10, PARTITION); }

// Call once, first thing in setup().
inline void begin() {
  for (int i = 0; i < 4 && !mounted; i++) {
    if (i) delay(250);
    mounted = mount();
  }
  // Still unreadable: keep trying until the power has settled, and only then
  // start a fresh one (the first start after moving from FAT lands here).
  while (!mounted && millis() < SETTLE_MS) {
    delay(250);
    mounted = mount();
  }
  if (!mounted) {
    LittleFS.format();
    mounted = mount();
    formatted = mounted;
  }
}

// Wipe and start again (System > Repair storage). Callers save what they hold in memory afterwards.
inline bool reformat() {
  LittleFS.end();
  mounted = false;
  LittleFS.format();
  mounted = mount();
  failStreak = 0;
  return mounted;
}

inline size_t size(const String &path) {
  if (!mounted || !LittleFS.exists(path)) return 0;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return 0;
  size_t n = f.size();
  f.close();
  return n;
}

inline std::vector<uint8_t> readFile(const String &path) {
  std::vector<uint8_t> out;
  if (!mounted || !LittleFS.exists(path)) return out;
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return out;
  out.resize(f.size());
  out.resize(f.read(out.data(), out.size()));
  f.close();
  return out;
}

// True when the file is `wantSize` long and holds `data` at `offset`.
inline bool matches(const String &path, size_t offset, const void *data, size_t n, size_t wantSize) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  bool ok = f.size() == wantSize && f.seek(offset);
  uint8_t buf[256];
  for (size_t done = 0; ok && done < n;) {
    size_t k = n - done < sizeof(buf) ? n - done : sizeof(buf);
    ok = f.read(buf, k) == k && !memcmp(buf, (const uint8_t *)data + done, k);
    done += k;
  }
  f.close();
  return ok;
}

// Replace a whole file. Written beside the old one, checked, then swapped in, so
// a power cut at any point leaves a complete file.
inline bool writeFile(const String &path, const void *data, size_t n) {
  if (!mounted) return false;
  String tmp = path + ".new";
  bool ok = false;
  File f = LittleFS.open(tmp, FILE_WRITE);
  if (f) {
    ok = f.write((const uint8_t *)data, n) == n;
    f.close();
  }
  ok = ok && matches(tmp, 0, data, n, n);
  if (ok && !LittleFS.rename(tmp, path)) {
    LittleFS.remove(path);
    ok = LittleFS.rename(tmp, path);
  }
  if (!ok) LittleFS.remove(tmp);
  return note(ok);
}

// Add to the end of a file (created if missing).
inline bool appendFile(const String &path, const void *data, size_t n) {
  if (!mounted) return false;
  size_t before = size(path);
  bool ok = false;
  File f = LittleFS.open(path, FILE_APPEND);
  if (f) {
    ok = f.write((const uint8_t *)data, n) == n;
    f.close();
  }
  return note(ok && matches(path, before, data, n, before + n));
}

// Overwrite part of an existing file without changing its length.
inline bool patchFile(const String &path, size_t offset, const void *data, size_t n) {
  if (!mounted) return false;
  size_t total = size(path);
  if (offset + n > total) return note(false);
  bool ok = false;
  File f = LittleFS.open(path, "r+");
  if (f) {
    ok = f.seek(offset) && f.write((const uint8_t *)data, n) == n;
    f.close();
  }
  return note(ok && matches(path, offset, data, n, total));
}

inline bool remove(const String &path) { return mounted && LittleFS.exists(path) && LittleFS.remove(path); }

// One line for the settings menu / console.
inline String summary() {
  if (!mounted) return "not available";
  if (!healthy()) return "PROBLEM: " + String((unsigned long)failures) + " failed saves";
  return failures ? "OK (" + String((unsigned long)failures) + " retried saves)" : String("OK");
}

}  // namespace Storage
