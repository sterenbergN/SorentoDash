// Settings backup and restore through the phone page: one JSON file with every
// saved setting (horn names, calibration, tank, trip, service items...) and
// the drive history files.
//
// A restore is checked, saved as /restore.json and applied at the next start,
// before anything else reads the settings; nothing is half-applied while the
// OBD task is running.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <functional>
#include <vector>
#include "EventLog.h"
#include "Health.h"
#include "History.h"
#include "Storage.h"

namespace Backup {

constexpr const char *FORMAT = "SorentoDash-backup";
constexpr const char *STAGED = "/restore.json";

// Every saved setting worth keeping: namespace, key, type
// (i int, u byte, f float, b bool, s string). Left out on purpose: the cached
// sound board file list (re-read at start) and the demo car switch.
struct Key {
  const char *ns, *key;
  char type;
};
constexpr Key KEYS[] = {
    {"dash", "volp", 'i'},     {"dash", "bright", 'i'},  {"dash", "leds", 'b'},    {"dash", "tint", 'i'},
    {"dash", "dimAfter", 'i'}, {"dash", "dimPct", 'i'},  {"dash", "sel", 's'},     {"dash", "horns", 's'},
    {"obd", "odo", 'f'},       {"obd", "tankcap", 'f'},  {"obd", "kfull", 'b'},    {"obd", "kstart", 'f'},
    {"obd", "kinc", 'b'},      {"obd", "disp", 'f'},     {"obd", "cal", 'f'},      {"obd", "calN", 'i'},
    {"obd", "lfEst", 'f'},     {"obd", "lfPump", 'f'},   {"obd", "lfWhy", 'u'},    {"obd", "ro", 'b'},
    {"obd", "addr", 's'},      {"obd", "tmi", 'f'},      {"obd", "tgal", 'f'},     {"obd", "tsec", 'f'},
    {"obd", "kmi", 'f'},       {"obd", "kgal", 'f'},     {"obd", "ksec", 'f'},     {"maint", "offset", 'f'},
    {"maint", "items", 's'},   {"maint", "aT", 'f'},     {"maint", "aR", 'f'},     {"obd", "dcal", 'f'},     {"obd", "ogap", 'b'},
};
constexpr const char *NAMESPACES[] = {"dash", "obd", "maint"};

// Files kept on the flash (history and health) and their record sizes (a restored file must be whole records).
struct HistFile {
  const char *name;
  size_t record;
};
constexpr HistFile FILES[] = {
    {"drives", sizeof(DriveRec)},
    {"fills", sizeof(FillRec)},
    {"speed", sizeof(float) * (History::BANDS * 3 + 2)},
    {"health", Health::STATE_SIZE}};

// ---- base64
inline String encode64(const std::vector<uint8_t> &in) {
  static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  out.reserve((in.size() + 2) / 3 * 4);
  for (size_t i = 0; i < in.size(); i += 3) {
    uint32_t v = in[i] << 16 | (i + 1 < in.size() ? in[i + 1] << 8 : 0) | (i + 2 < in.size() ? in[i + 2] : 0);
    out += T[v >> 18 & 63];
    out += T[v >> 12 & 63];
    out += i + 1 < in.size() ? T[v >> 6 & 63] : '=';
    out += i + 2 < in.size() ? T[v & 63] : '=';
  }
  return out;
}

inline bool decode64(const char *in, std::vector<uint8_t> &out) {
  out.clear();
  uint32_t v = 0;
  int bits = 0;
  for (const char *p = in; *p && *p != '='; p++) {
    char c = *p;
    int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
          : c == '+' ? 62 : c == '/' ? 63 : -1;
    if (d < 0) return false;
    v = v << 6 | d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(v >> bits & 0xFF);
    }
  }
  return true;
}

// ---- backup
// `files(name)` returns a history file's bytes (History reads it under its lock).
inline String make(const char *fw, std::function<std::vector<uint8_t>(const char *)> files) {
  JsonDocument doc;
  doc["format"] = FORMAT;
  doc["version"] = 1;
  doc["fw"] = fw;
  JsonObject prefs = doc["prefs"].to<JsonObject>();
  for (const char *ns : NAMESPACES) {
    Preferences p;
    p.begin(ns, true);
    JsonObject o = prefs[ns].to<JsonObject>();
    for (const Key &k : KEYS) {
      if (strcmp(k.ns, ns) || !p.isKey(k.key)) continue;
      switch (k.type) {
        case 'i': o[k.key] = p.getInt(k.key); break;
        case 'u': o[k.key] = p.getUChar(k.key); break;
        case 'f': o[k.key] = p.getFloat(k.key); break;
        case 'b': o[k.key] = p.getBool(k.key); break;
        case 's': o[k.key] = p.getString(k.key); break;
      }
    }
    p.end();
  }
  JsonObject fo = doc["files"].to<JsonObject>();
  for (const HistFile &h : FILES) fo[h.name] = encode64(files(h.name));
  String out;
  serializeJson(doc, out);
  return out;
}

// ---- restore
// Check an uploaded backup and keep it for the next start. Returns an error
// for the person restoring, or "" when it's ready.
inline String stage(const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return "That file isn't a Sorento Dash backup (not readable).";
  if (doc["format"] != FORMAT) return "That file isn't a Sorento Dash backup.";
  if (!doc["prefs"].is<JsonObject>()) return "The backup has no settings in it.";
  std::vector<uint8_t> bytes;
  for (const HistFile &h : FILES) {
    const char *b64 = doc["files"][h.name] | "";
    if (!decode64(b64, bytes) || bytes.size() % h.record) return "The backup's drive history is damaged.";
  }
  if (!Storage::writeFile(STAGED, body.c_str(), body.length())) return "Couldn't save the backup on the display.";
  return "";
}

// At start, before settings are read: apply a staged backup, if any.
inline void applyStaged() {
  std::vector<uint8_t> raw = Storage::readFile(STAGED);
  if (raw.empty()) return;
  Storage::remove(STAGED);  // whatever happens, only try once
  JsonDocument doc;
  if (deserializeJson(doc, (const char *)raw.data(), raw.size()) || doc["format"] != FORMAT) {
    events.add("Restore failed: backup unreadable");
    return;
  }
  JsonObject prefs = doc["prefs"];
  for (const char *ns : NAMESPACES) {
    Preferences p;
    p.begin(ns, false);
    JsonObject o = prefs[ns];
    for (const Key &k : KEYS) {
      if (strcmp(k.ns, ns)) continue;
      JsonVariant v = o[k.key];
      if (v.isNull()) {  // not set in the backup: back to the default
        p.remove(k.key);
        continue;
      }
      switch (k.type) {
        case 'i': p.putInt(k.key, v.as<int>()); break;
        case 'u': p.putUChar(k.key, v.as<uint8_t>()); break;
        case 'f': p.putFloat(k.key, v.as<float>()); break;
        case 'b': p.putBool(k.key, v.as<bool>()); break;
        case 's': p.putString(k.key, v.as<String>()); break;
      }
    }
    p.end();
  }
  std::vector<uint8_t> bytes;
  int drives = 0;
  for (const HistFile &h : FILES) {
    String path = String("/") + h.name + ".bin";
    Storage::remove(path);
    if (!decode64(doc["files"][h.name] | "", bytes) || bytes.empty()) continue;
    if (!Storage::writeFile(path, bytes.data(), bytes.size())) continue;
    if (!strcmp(h.name, "drives")) drives = bytes.size() / h.record;
  }
  Storage::remove(Health::DRIVE_PATH);  // a half-recorded drive belongs to the old health state
  events.add("Restored a backup from v%s (%d drives)", (const char *)(doc["fw"] | "?"), drives);
}

}  // namespace Backup
