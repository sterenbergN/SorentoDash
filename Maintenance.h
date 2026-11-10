// Mileage-based service reminders.
//
// The car's own odometer isn't available over OBD-II, so the display counts
// miles itself (ObdData::odoMiles, "tracked" miles). The user enters the real
// odometer once and we keep an offset, so everything is shown in odometer
// terms. Service history is stored in tracked miles so it stays correct if the
// odometer is re-entered later.
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <vector>
#include "EventLog.h"

struct ServiceItem {
  String name;
  uint32_t interval;       // miles
  float lastTracked = NAN; // tracked miles when last done; NAN = never logged
};

class Maintenance {
public:
  static constexpr float DUE_SOON_MI = 300;

  std::vector<ServiceItem> items;

  void begin() {
    p.begin("maint", false);
    odoSet = p.isKey("offset");
    offset = p.getFloat("offset", 0);
    anchorSet = p.isKey("aT") && p.isKey("aR");
    anchorTracked = p.getFloat("aT", 0);
    anchorReal = p.getFloat("aR", 0);
    parse(p.getString("items", ""));
    if (items.empty()) {
      // Typical intervals for a 2006 Sorento 3.5 V6 - editable on the phone.
      items = {{"Oil change", 5000}, {"Tire rotation", 7500}, {"Engine air filter", 15000},
               {"Cabin air filter", 15000}, {"Brake fluid", 30000}, {"Transmission fluid", 30000},
               {"Coolant", 60000}, {"Spark plugs", 60000}};
      save();
    }
  }

  // Back to factory defaults (development/testing).
  void reset() {
    p.clear();
    p.end();
    items.clear();
    begin();
  }

  bool odometerKnown() const { return odoSet; }
  float odometer(float tracked) const { return tracked + offset; }
  // keepAnchor: a correction too soon after the last one to learn from keeps
  // counting from the earlier starting point.
  void setOdometer(float value, float tracked, bool keepAnchor = false) {
    float newOffset = value - tracked;
    if (odoSet) {
      // Correcting the odometer must not move logged services: keep each one
      // at the odometer reading it was recorded at.
      float shift = newOffset - offset;
      for (auto &it : items)
        if (!isnan(it.lastTracked)) it.lastTracked -= shift;
      save();
    }
    offset = newOffset;
    odoSet = true;
    p.putFloat("offset", offset);
    if (!keepAnchor || !anchorSet) {
      anchorTracked = tracked;
      anchorReal = value;
      anchorSet = true;
      p.putFloat("aT", tracked);
      p.putFloat("aR", value);
    }
    events.add("Odometer set to %.0f mi", value);
  }

  // Where the odometer was last set: the display's own mile count then, and
  // the real reading then (for learning the speed correction).
  bool anchorKnown() const { return anchorSet && odoSet; }
  float anchorTrackedMiles() const { return anchorTracked; }
  float anchorOdometer() const { return anchorReal; }
  // The speed correction was changed by hand: miles counted until now used the
  // old one, so the next odometer correction only sets a new starting point.
  void forgetAnchor() {
    anchorSet = false;
    p.remove("aT");
    p.remove("aR");
  }

  // Miles until due (negative = overdue), NAN if never logged.
  float remaining(const ServiceItem &s, float tracked) const {
    if (isnan(s.lastTracked)) return NAN;
    return s.lastTracked + s.interval - tracked;
  }

  void markDone(int i, float tracked) {
    if (i < 0 || i >= (int)items.size()) return;
    items[i].lastTracked = tracked;
    save();
    if (odoSet) events.add("Service logged: %s at %.0f mi", items[i].name.c_str(), odometer(tracked));
    else events.add("Service logged: %s", items[i].name.c_str());
  }

  // Most urgent logged item (smallest remaining), or -1.
  int mostUrgent(float tracked) const {
    int best = -1;
    for (size_t i = 0; i < items.size(); i++) {
      float r = remaining(items[i], tracked);
      if (!isnan(r) && (best < 0 || r < remaining(items[best], tracked))) best = i;
    }
    return best;
  }

  void save() {
    String s;
    for (auto &it : items) {
      String n = it.name;
      n.replace("\t", " ");
      n.replace("\n", " ");
      s += n + "\t" + String(it.interval) + "\t" + (isnan(it.lastTracked) ? String("") : String(it.lastTracked, 1)) + "\n";
    }
    p.putString("items", s);
  }

private:
  Preferences p;
  float offset = 0, anchorTracked = 0, anchorReal = 0;
  bool odoSet = false, anchorSet = false;

  void parse(const String &s) {
    items.clear();
    int start = 0;
    while (start < (int)s.length()) {
      int end = s.indexOf('\n', start);
      if (end < 0) end = s.length();
      String line = s.substring(start, end);
      int a = line.indexOf('\t'), b = line.indexOf('\t', a + 1);
      if (a > 0 && b > a) {
        String last = line.substring(b + 1);
        items.push_back({line.substring(0, a), (uint32_t)line.substring(a + 1, b).toInt(),
                         last.length() ? last.toFloat() : NAN});
      }
      start = end + 1;
    }
  }
};
