// Persistent settings (NVS): user preferences, per-horn names/order/visibility,
// and a cached copy of the sound board's file list so the real horn names are
// available immediately at power-up, before the Audio FX has finished booting.
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <vector>

struct HornMeta {
  String key;   // raw 8.3 file name from the Audio FX, e.g. "T02     WAV"
  String name;  // user-given name, empty = derive from the file name
  bool hidden = false;
  String color;  // "#rrggbb", empty = automatic palette colour
};

class Settings {
public:
  int volumePct = 80;      // 0..100, perceptual (see volumeRaw)
  int brightnessPct = 90;  // 10..100
  bool leds = true;
  int tint = 1;  // horn screen background: 0 black, 1 subtle, 2 bold
  int dimAfterSec = 60;  // dim the screen after this long without input (0 = never)
  int dimPct = 20;       // brightness when dimmed (never brighter than the normal setting)
  int homePage = 0;      // car page shown at start-up: the one that was up last
  int hiddenPages = 0;   // bit per car page left out of the home carousel
  String selectedKey;
  std::vector<HornMeta> horns;  // user order
  std::vector<String> cachedFiles;

  void begin() {
    p.begin("dash", false);
    volumePct = p.getInt("volp", volumePct);
    brightnessPct = p.getInt("bright", brightnessPct);
    leds = p.getBool("leds", leds);
    tint = p.getInt("tint", tint);
    dimAfterSec = p.getInt("dimAfter", dimAfterSec);
    dimPct = p.getInt("dimPct", dimPct);
    homePage = p.getInt("page", homePage);
    hiddenPages = p.getInt("pages", hiddenPages);
    selectedKey = p.getString("sel", "");
    parseHorns(p.getString("horns", ""));
    cachedFiles = split(p.getString("files", ""), '\n');
  }

  void saveVolume() { p.putInt("volp", volumePct); }
  void saveBrightness() { p.putInt("bright", brightnessPct); }
  void saveLeds() { p.putBool("leds", leds); }
  void saveTint() { p.putInt("tint", tint); }
  void saveDim() {
    p.putInt("dimAfter", dimAfterSec);
    p.putInt("dimPct", dimPct);
  }
  void saveHomePage(int page) {
    if (page == homePage) return;
    homePage = page;
    p.putInt("page", page);
  }
  void saveHiddenPages() { p.putInt("pages", hiddenPages); }
  void saveSelected(const String &key) {
    if (key == selectedKey) return;
    selectedKey = key;
    p.putString("sel", key);
  }

  void saveHorns() {
    String s;
    for (auto &h : horns) {
      String n = h.name;
      n.replace("\t", " ");
      n.replace("\n", " ");
      s += h.key + "\t" + n + "\t" + (h.hidden ? "1" : "0") + "\t" + h.color + "\n";
    }
    p.putString("horns", s);
  }

  // Returns true if the list differs from the cached one (and stores it).
  bool updateCachedFiles(const std::vector<String> &files) {
    if (files == cachedFiles) return false;
    cachedFiles = files;
    String s;
    for (auto &f : files) s += f + "\n";
    p.putString("files", s);
    return true;
  }

  HornMeta *find(const String &key) {
    for (auto &h : horns)
      if (h.key == key) return &h;
    return nullptr;
  }

  // Sound board volume (0..204, 0.5 dB steps) for a 0..100% setting. The
  // top ~30 dB is the useful range; anything quieter is inaudible in a car.
  static int volumeRaw(int pct) {
    if (pct <= 0) return 0;
    return 204 - (100 - pct) * 6 / 10;
  }

private:
  Preferences p;

  static std::vector<String> split(const String &s, char sep) {
    std::vector<String> out;
    int start = 0;
    while (start < (int)s.length()) {
      int end = s.indexOf(sep, start);
      if (end < 0) end = s.length();
      if (end > start) out.push_back(s.substring(start, end));
      start = end + 1;
    }
    return out;
  }

  void parseHorns(const String &s) {
    horns.clear();
    for (auto &line : split(s, '\n')) {
      int a = line.indexOf('\t'), b = line.indexOf('\t', a + 1);
      if (a < 0 || b < 0) continue;
      int c = line.indexOf('\t', b + 1);  // colour field (added later)
      String hidden = c < 0 ? line.substring(b + 1) : line.substring(b + 1, c);
      horns.push_back({line.substring(0, a), line.substring(a + 1, b), hidden == "1", c < 0 ? "" : line.substring(c + 1)});
    }
  }
};
