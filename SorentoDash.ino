// Sorento Dash - rotary horn selector and trip computer for the CrowPanel
// 1.28" ESP32-S3 knob display.
//
// Car pages (home): turn or swipe = next page, press = this page's details
//                (fuel details, fill-up, fuel trims, codes, service log, charts),
//                hold = settings, double-click = horns.
// Horns screen:  turn = choose a horn, press = play (press again to stop),
//                hold = settings. Returns home after a short idle.
// Menu:          turn = choose, press = open, long press = back.
//
// Horns come from the Adafruit Audio FX board's file list over UART. Names,
// colours, order and visibility are edited from a phone (Menu > Phone setup).
// Car data comes from a Bluetooth LE ELM327 OBD-II adapter (see Obd.h).
#include "Board.h"
#include "EventLog.h"
#include "Backup.h"
#include "AudioFX.h"
#include "Fonts.h"
#include "Settings.h"
#include "WebSetup.h"
#include "UiTypes.h"
#include "Obd.h"
#include "ObdCodes.h"
#include "Touch.h"
#include "Maintenance.h"
#include "RfCapture.h"
#include "History.h"
#include "Menu.h"
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <vector>

LGFX lcd;
LGFX_Sprite canvas(&lcd);
Adafruit_NeoPixel ring(LED_RING_COUNT, PIN_LED_RING, NEO_GRB + NEO_KHZ800);
AudioFX fx;
Settings cfg;
WebSetup web;
Obd obd;
Touch touch;
Maintenance maint;
RfCapture rf;
History hist;
Health health;

#define FW_VERSION "1.21"

// ---------------------------------------------------------------- tuning
constexpr uint32_t LONG_PRESS_MS = 600;
constexpr uint32_t HOLD_REPEAT_MS = 450;  // keep holding in settings: back another level each time
constexpr uint32_t HORNS_TIMEOUT_MS = 20000;  // horn screen falls back to the stats
constexpr uint32_t DOUBLE_CLICK_MS = 380;    // second press within this = double-click
constexpr uint32_t WIFI_IDLE_OFF_MS = 120000;
constexpr int PCT_STEP = 5;

// ---------------------------------------------------------------- colours
static uint16_t rgb(uint32_t hex) {
  return lgfx::color565((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}
static uint32_t lerpHex(uint32_t a, uint32_t b, float t) {
  t = constrain(t, 0.0f, 1.0f);
  auto ch = [&](int s) { return (uint32_t)(((a >> s) & 0xFF) + (((int)((b >> s) & 0xFF) - (int)((a >> s) & 0xFF)) * t)) << s; };
  return ch(16) | ch(8) | ch(0);
}

const uint32_t ACCENTS[] = {0xFF8A1F, 0x22D3EE, 0xA3E635, 0xF472B6, 0xFACC15, 0xA78BFA, 0xF87171, 0x2DD4BF};
constexpr int N_ACCENTS = sizeof(ACCENTS) / sizeof(ACCENTS[0]);
constexpr uint32_t C_TRACK = 0x1C1F24, C_TICK = 0x3A3F47, C_DIM = 0x6B7280, C_TEXT = 0xF5F5F5, C_WARN = 0xF59E0B;
constexpr uint32_t C_MENU = 0xE5E7EB, C_VOLUME = 0xFF8A1F, C_BRIGHT = 0xFACC15, C_WIFI = 0x22D3EE;
constexpr uint32_t C_GOOD = 0x34D399, C_BAD = 0xF87171, C_COLD = 0x60A5FA;
constexpr uint32_t C_BLUE = 0x3987E5;  // chart bars/lines (same as the phone charts)

// ---------------------------------------------------------------- horns
struct Horn {
  String label;
  String key;      // raw 8.3 file name, used to remember names and selection
  int track;       // Audio FX track number, -1 in demo mode
  uint32_t color;  // accent colour
};
std::vector<Horn> horns;    // visible horns, in the user's order
std::vector<String> files;  // playable files known on the sound board
bool demoMode = true;
uint32_t shownListVersion = 0;

const char *DEMO_HORNS[] = {"Train", "La Cucaracha", "Dixie", "Air Horn", "Big Rig", "Ship", "Duck", "Clown Car"};

// "T01HOLD WAV" -> "Horn 2"; "02_BIG_RIG" -> "Big Rig"
static String prettyName(const String &raw, int fallbackNum) {
  String base = raw.substring(0, 8);
  base.trim();
  base.toUpperCase();
  // Strip Audio FX trigger-mode naming (T00, T01HOLDL, T02LATCH ...).
  if (base.length() >= 3 && base[0] == 'T' && isdigit(base[1]) && isdigit(base[2])) {
    base = base.substring(3);
    for (const char *m : {"HOLDL", "LATCH", "NEXT", "RAND", "HOLD"})
      if (base.startsWith(m)) { base = base.substring(strlen(m)); break; }
  }
  // Leading digits/underscores are just for ordering.
  while (base.length() && (isdigit(base[0]) || base[0] == '_' || base[0] == '-')) base.remove(0, 1);
  if (!base.length()) return "Horn " + String(fallbackNum);

  String out;
  bool startWord = true;
  for (char c : base) {
    if (c == '_' || c == '-') { out += ' '; startWord = true; continue; }
    out += startWord ? (char)toupper(c) : (char)tolower(c);
    startWord = false;
  }
  return out;
}

// "T02     WAV" -> "T02.WAV"
static String displayFileName(const String &raw) {
  String base = raw.substring(0, 8), ext = raw.substring(8);
  base.trim();
  return base + "." + ext;
}

// Default name for a file: from its name, or "Horn N" by file-name order.
static String autoName(const String &key) {
  std::vector<String> sorted = files;
  std::sort(sorted.begin(), sorted.end());
  int n = std::find(sorted.begin(), sorted.end(), key) - sorted.begin() + 1;
  return prettyName(key, n);
}

static int trackOf(const String &key) {
  if (fx.fileCount() > 0) {
    for (int i = 0; i < fx.fileCount(); i++)
      if (key == fx.file(i).name) return i;
    return -1;
  }
  // Not listed yet this boot: the cached list has the board's order.
  for (size_t i = 0; i < cfg.cachedFiles.size(); i++)
    if (cfg.cachedFiles[i] == key) return i;
  return -1;
}

// Refresh the known file list from the sound board (or the cache) and make
// sure every file has an entry in the user's horn order.
static void syncFiles() {
  std::vector<String> raw;
  bool live = fx.fileCount() > 0;
  if (live) {
    for (int i = 0; i < fx.fileCount(); i++) raw.push_back(fx.file(i).name);
    cfg.updateCachedFiles(raw);
  } else {
    raw = cfg.cachedFiles;
  }
  files.clear();
  for (auto &k : raw)
    if (k.endsWith("OGG") || k.endsWith("WAV")) files.push_back(k);

  bool changed = false;
  if (live) {  // forget files that are gone from the board
    size_t before = cfg.horns.size();
    cfg.horns.erase(std::remove_if(cfg.horns.begin(), cfg.horns.end(),
                                   [](const HornMeta &h) { return std::find(files.begin(), files.end(), h.key) == files.end(); }),
                    cfg.horns.end());
    changed = cfg.horns.size() != before;
  }
  std::vector<String> fresh;
  for (auto &k : files)
    if (!cfg.find(k)) fresh.push_back(k);
  std::sort(fresh.begin(), fresh.end());
  for (auto &k : fresh) cfg.horns.push_back({k, "", false, ""}), changed = true;
  if (changed) cfg.saveHorns();
}

static uint32_t parseColor(const String &s, uint32_t fallback) {
  if (s.length() != 7 || s[0] != '#') return fallback;
  return strtoul(s.c_str() + 1, nullptr, 16);
}

// ---------------------------------------------------------------- encoder
volatile int32_t encDetents = 0;
volatile int8_t encQuad = 0;
volatile uint8_t encLast = 0;
portMUX_TYPE encMux = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR static const int8_t ENC_TABLE[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void IRAM_ATTR encoderISR() {
  uint8_t s = ((uint8_t)digitalRead(PIN_ENC_A) << 1) | (uint8_t)digitalRead(PIN_ENC_B);
  int8_t m = ENC_TABLE[(encLast << 2) | s];
  portENTER_CRITICAL_ISR(&encMux);
  encLast = s;
  if (m) {
    encQuad += m;
    if (encQuad >= 4) { encDetents = encDetents + 1; encQuad -= 4; }
    else if (encQuad <= -4) { encDetents = encDetents - 1; encQuad += 4; }
  }
  portEXIT_CRITICAL_ISR(&encMux);
}

static int32_t takeDetents() {
  portENTER_CRITICAL(&encMux);
  int32_t d = encDetents;
  encDetents = 0;
  portEXIT_CRITICAL(&encMux);
  return d;
}

// ---------------------------------------------------------------- button
struct Button {
  bool raw = false, stable = false, longFired = false;
  uint32_t changedAt = 0, pressedAt = 0, repeatAt = 0;
  // Short press fires on release; long press fires while still held, then
  // PRESS_REPEAT keeps firing while the knob stays down.
  Press poll(uint32_t now) {
    bool r = digitalRead(PIN_ENC_SW) == LOW;
    if (r != raw) { raw = r; changedAt = now; }
    if (now - changedAt > 15 && stable != raw) {
      stable = raw;
      if (stable) { pressedAt = now; longFired = false; }
      else if (!longFired) return PRESS_SHORT;
    }
    if (stable && !longFired && now - pressedAt >= LONG_PRESS_MS) {
      longFired = true;
      repeatAt = now + HOLD_REPEAT_MS;
      return PRESS_LONG;
    }
    if (stable && longFired && (int32_t)(now - repeatAt) >= 0) {
      repeatAt = now + HOLD_REPEAT_MS;
      return PRESS_REPEAT;
    }
    return PRESS_NONE;
  }
} button;

// ---------------------------------------------------------------- state
Mode mode = MODE_CAR;  // home screen: car stats
Wheel hornWheel;
Menu menu;
uint32_t lastInput = 0, selChangedAt = 0, tripResetAt = 0;
bool dimmed = false;
int playingHorn = -1;  // index into horns

enum CarPage { CP_ECONOMY, CP_TRIP, CP_ENGINE, CP_CODES, CP_HEALTH, CP_SERVICE, CP_HISTORY, CP_COUNT };
constexpr int HIST_CHARTS = 3;  // MPG by speed, last drives, MPG trend
int histChart = 0;
int carPage = CP_ECONOMY;
ObdData car;  // snapshot, refreshed each frame

// Demo-mode playback simulation.
bool demoPlaying = false;
uint32_t demoStart = 0;
constexpr uint32_t DEMO_LEN_MS = 2500;

float fillGallons = 0;

// Number editor (MODE_NUMBER) and confirmation (MODE_CONFIRM) state.
MenuItem editItem;
float editValue = 0;
String confirmQuestion, confirmSubject;
// Text screen (MODE_TEXT): an info item's full explanation, scrolled with the knob.
String textTitle;
std::vector<String> textLines;
int textScroll = 0;
std::function<void()> confirmAction;
uint32_t lastDetentAt = 0;
int otaPct = 0;
bool otaOk = false;

// Short confirmation pill drawn over any screen.
String toastText, pendingToast;  // pendingToast: a number editor's save can say more than "Saved"
uint32_t toastUntil = 0;
static void showToast(const String &t) {
  toastText = t;
  toastUntil = millis() + 1800;
}

static int selected() { return hornWheel.index(horns.size()); }
static uint32_t accentFor(int i) { return i >= 0 && i < (int)horns.size() ? horns[i].color : ACCENTS[0]; }
static bool isPlaying() { return demoMode ? demoPlaying : fx.isPlaying(); }
static float playProgress() {
  if (demoMode) return demoPlaying ? min(1.0f, (millis() - demoStart) / (float)DEMO_LEN_MS) : 0;
  return fx.progress();
}

static void rebuildHorns() {
  String keep = horns.empty() ? cfg.selectedKey : horns[selected()].key;
  syncFiles();
  horns.clear();
  demoMode = files.empty();
  if (demoMode) {
    for (const char *n : DEMO_HORNS) horns.push_back({n, n, -1, ACCENTS[horns.size() % N_ACCENTS]});
  } else {
    for (auto &m : cfg.horns) {
      if (m.hidden || std::find(files.begin(), files.end(), m.key) == files.end()) continue;
      uint32_t c = parseColor(m.color, ACCENTS[horns.size() % N_ACCENTS]);
      horns.push_back({m.name.length() ? m.name : autoName(m.key), m.key, trackOf(m.key), c});
    }
  }
  int sel = 0;
  for (size_t i = 0; i < horns.size(); i++)
    if (horns[i].key == keep) sel = i;
  hornWheel.jump(sel);
  playingHorn = -1;
}

// Engine problems worth interrupting for (the dash already shows the MIL).
static const char *carAlert(const ObdData &d) {
  if (!d.live()) return nullptr;
  if (!isnan(d.coolantF) && d.coolantF >= 230) return "ENGINE HOT";
  if (!isnan(d.volts) && !isnan(d.rpm) && d.rpm > 500 && d.volts < 12.6f) return "NOT CHARGING";
  if (!isnan(d.volts) && d.volts > 15.3f) return "HIGH VOLTAGE";
  return nullptr;
}

// Most urgent service that is due within DUE_SOON_MI (index), or -1.
static int serviceDue() {
  int i = maint.mostUrgent(car.odoMiles);
  if (i < 0) return -1;
  return maint.remaining(maint.items[i], car.odoMiles) <= Maintenance::DUE_SOON_MI ? i : -1;
}

// ---------------------------------------------------------------- actions
static uint8_t backlightFor(int pct) { return map(constrain(pct, 10, 100), 10, 100, 20, 255); }
static uint8_t backlightLevel() { return backlightFor(cfg.brightnessPct); }
static uint8_t dimLevel() { return min(backlightLevel(), backlightFor(cfg.dimPct)); }

// "Dim after" choices, in seconds (0 = never).
static const int DIM_CHOICES[] = {15, 30, 60, 120, 300, 600, 0};
static String dimAfterText(int sec) {
  if (sec <= 0) return "Never";
  return sec < 60 ? String(sec) + " sec" : String(sec / 60) + " min";
}
static void setBacklight(uint8_t v) { ledcWrite(PIN_BACKLIGHT, v); }
static void applyVolume() { fx.setVolumeTarget(Settings::volumeRaw(cfg.volumePct)); }

static void playHorn(int i) {
  if (i < 0 || i >= (int)horns.size()) return;
  if (isPlaying() && playingHorn == i) {
    if (demoMode) demoPlaying = false;
    else fx.stop();
    return;
  }
  playingHorn = i;
  if (demoMode) {
    demoPlaying = true;
    demoStart = millis();
  } else if (horns[i].track >= 0) {
    fx.play(horns[i].track);
  }
  Serial.printf("PLAY %d: %s\n", i, horns[i].label.c_str());
}

static void enterMode(Mode m) {
  if (mode == MODE_VOLUME && m != MODE_VOLUME) cfg.saveVolume();
  if (mode == MODE_BRIGHTNESS && m != MODE_BRIGHTNESS) cfg.saveBrightness();
  if (m == MODE_WIFI) web.start();
  if (m == MODE_MENU && !menu.open()) menu.openRoot();
  if (m == MODE_CAR || m == MODE_HORNS) menu.stack.clear();
  if (m == MODE_FILLUP) {
    // Start from what the trip computer thinks was used since the last fill-up.
    float used = car.tankUsed();
    fillGallons = used >= 1 ? roundf(used * 10) / 10 : 10.0f;
  }
  mode = m;
}

// Leave a screen: back to the settings menu if we came from it, else home.
static void backOrHome() {
  if (mode == MODE_VOLUME) cfg.saveVolume();
  if (mode == MODE_BRIGHTNESS) cfg.saveBrightness();
  if (menu.open()) {
    mode = MODE_MENU;
    menu.refresh();
  } else {
    if (mode == MODE_FILLUP) carPage = CP_TRIP;
    enterMode(MODE_CAR);
  }
}

// Horns <-> car pages as one carousel (for swipes).
static void stepPage(int dir) {
  if (mode == MODE_HORNS) {
    carPage = dir > 0 ? 0 : CP_COUNT - 1;
    enterMode(MODE_CAR);
  } else if (mode == MODE_CAR) {
    carPage += dir;
    if (carPage < 0 || carPage >= CP_COUNT) enterMode(MODE_HORNS);
  } else if (mode == MODE_HISTORY) {
    histChart = ((histChart + dir) % HIST_CHARTS + HIST_CHARTS) % HIST_CHARTS;
  } else if (mode == MODE_DETAIL) {
    mode = MODE_CAR;
    stepPage(dir);
  } else if (mode == MODE_MENU || mode == MODE_TEXT) {
    enterMode(MODE_CAR);  // swipe = leave settings
  } else if (mode == MODE_FILLUP || mode == MODE_NUMBER || mode == MODE_CONFIRM) {
    // A swipe backs out of an edit screen without saving.
    showToast("Cancelled");
    backOrHome();
  }
}

// ---------------------------------------------------------------- phone API
static void putNum(JsonObject o, const char *k, float v) {
  if (isnan(v)) o[k] = nullptr;
  else o[k] = roundf(v * 100) / 100;
}

static void putTrip(JsonObject o, const TripTotals &t) {
  o["mi"] = t.miles;
  o["gal"] = t.gallons;
  o["sec"] = t.seconds;
  putNum(o, "mpg", t.mpg() > 0 ? t.mpg() : NAN);
}

// Speed correction, in percent (+2.0 = speed was reading 2% low and is now raised).
static float speedCorrectionPct() { return (obd.distanceCal() - 1) * 100; }
static String speedCorrectionText() {
  float p = speedCorrectionPct();
  return fabsf(p) < 0.05f ? String("none") : (p > 0 ? "+" : "") + String(p, 1) + "%";
}

static void changeSpeedCal(float cal) {
  float k = cal / obd.distanceCal();
  obd.setDistanceCal(cal);
  health.rescaleSpeed(k);
}

// Set by hand (e.g. against a phone's GPS speed).
static void setSpeedCorrectionPct(float pct) {
  changeSpeedCal(1 + pct / 100);
  maint.forgetAnchor();
  events.add("Speed correction set by hand to %s", speedCorrectionText().c_str());
}

// The real odometer reading was entered. If the display has counted enough
// miles since the last time, with nothing missed, learn how far off its speed
// reading is. Returns what happened, for a toast.
static String setOdometerReading(float real) {
  float tracked = obd.snapshot().odoMiles;
  String result = "Odometer set";
  bool keepAnchor = false;
  if (maint.anchorKnown()) {
    float counted = tracked - maint.anchorTrackedMiles();
    float driven = real - maint.anchorOdometer();
    float shown = maint.odometer(tracked);
    if (counted < 0) {  // the display's mile count was reset since: start over
      events.add("Odometer corrected %.0f -> %.0f mi (mile count was reset; starting over)", shown, real);
    } else if (obd.distanceGap()) {
      events.add("Odometer corrected %.0f -> %.0f mi (some driving was missed, speed correction not changed)", shown, real);
    } else if (counted < 200) {
      events.add("Odometer corrected %.0f -> %.0f mi (%.0f mi counted so far; learns after 200+)", shown, real, counted);
      keepAnchor = true;
    } else if (driven / counted < 0.85f || driven / counted > 1.15f) {
      events.add("Odometer corrected %.0f -> %.0f mi (%.0f%% off is too much to be the speed reading; not learned)",
                 shown, real, (driven / counted - 1) * 100);
    } else {
      changeSpeedCal(obd.distanceCal() * driven / counted);
      result = "Speed corrected " + speedCorrectionText();
      events.add("Odometer corrected %.0f -> %.0f mi: counted %.0f mi, really %.0f -> speed correction now %s", shown,
                 real, counted, driven, speedCorrectionText().c_str());
    }
  }
  maint.setOdometer(real, tracked, keepAnchor);
  if (!keepAnchor) obd.clearDistanceGap();
  return result;
}

String webStateJson() {
  JsonDocument doc;
  doc["connected"] = fx.connected();
  doc["volume"] = cfg.volumePct;
  doc["brightness"] = cfg.brightnessPct;
  doc["leds"] = cfg.leds;
  doc["tint"] = cfg.tint;
  doc["dimAfter"] = cfg.dimAfterSec;
  doc["dimPct"] = cfg.dimPct;
  JsonArray arr = doc["horns"].to<JsonArray>();
  for (auto &m : cfg.horns) {
    if (std::find(files.begin(), files.end(), m.key) == files.end()) continue;
    JsonObject h = arr.add<JsonObject>();
    h["key"] = m.key;
    h["file"] = displayFileName(m.key);
    h["auto"] = autoName(m.key);
    h["name"] = m.name;
    h["hidden"] = m.hidden;
    h["color"] = m.color;
  }

  ObdData d = obd.snapshot();
  bool live = d.live();
  JsonObject o = doc["obd"].to<JsonObject>();
  o["state"] = d.stateText();
  o["adapter"] = d.adapter;
  o["sim"] = obd.simulating();
  o["readOnly"] = obd.isReadOnly();
  putNum(o, "mpg", live ? d.instMpg : NAN);
  putNum(o, "gph", live ? d.gph : NAN);
  putNum(o, "mph", live ? d.mph : NAN);
  putNum(o, "coolant", live ? d.coolantF : NAN);
  putNum(o, "volts", live ? d.volts : NAN);
  putTrip(o["trip"].to<JsonObject>(), d.trip);
  putTrip(o["tank"].to<JsonObject>(), d.tank);
  putNum(o, "range", d.rangeMiles());
  o["cal"] = roundf(d.fuelCal * 100) / 100;
  o["tankFull"] = d.tankFromFull;
  putNum(o, "tankStart", d.tankStartUsed);
  putNum(o, "level", d.tankFromFull ? d.tankFraction() * 100 : NAN);
  o["tankIncomplete"] = d.tankIncomplete;
  o["calFills"] = d.calFills;
  o["lastFill"] = (int)d.lastFill;
  o["lastFillEst"] = roundf(d.lastFillEstimate * 10) / 10;
  o["lastFillPumped"] = roundf(d.lastFillPumped * 10) / 10;
  o["disp"] = obd.displacementL();
  o["tankCap"] = d.tankCapacity;
  o["mil"] = d.milOn;
  o["codesRead"] = d.dtcsRead;
  JsonArray codes = o["codes"].to<JsonArray>();
  for (int i = 0; i < d.nDtcs; i++) {
    JsonObject c = codes.add<JsonObject>();
    String code = ObdData::dtcText(d.dtcs[i]);
    c["code"] = code;
    const char *t = dtcDescription(code);
    if (t) c["text"] = t;
  }
  o["trans"] = d.transText();
  o["transBusy"] = d.trans == ObdData::TR_READING;
  JsonArray tcodes = o["transCodes"].to<JsonArray>();
  for (int i = 0; i < d.nTransDtcs; i++) {
    JsonObject c = tcodes.add<JsonObject>();
    String code = ObdData::dtcText(d.transDtcs[i]);
    c["code"] = code;
    c["now"] = d.transNow[i];
    const char *t = dtcDescription(code);
    if (t) c["text"] = t;
  }

  doc["fw"] = String(FW_VERSION " (" __DATE__ ")");
  JsonObject mo = doc["maint"].to<JsonObject>();
  float tracked = d.odoMiles;
  putNum(mo, "odometer", maint.odometerKnown() ? maint.odometer(tracked) : NAN);
  mo["speedCal"] = roundf(speedCorrectionPct() * 10) / 10;
  putNum(mo, "sinceSet", maint.anchorKnown() && tracked >= maint.anchorTrackedMiles() ? tracked - maint.anchorTrackedMiles() : NAN);
  JsonArray items = mo["items"].to<JsonArray>();
  for (auto &it : maint.items) {
    JsonObject o = items.add<JsonObject>();
    o["name"] = it.name;
    o["interval"] = it.interval;
    putNum(o, "last", maint.odometerKnown() && !isnan(it.lastTracked) ? maint.odometer(it.lastTracked) : NAN);
    putNum(o, "remaining", maint.remaining(it, tracked));
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void webSaveHorns(const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  std::vector<HornMeta> ordered;
  for (JsonObject h : doc["horns"].as<JsonArray>()) {
    String key = h["key"] | "";
    if (!cfg.find(key)) continue;
    ordered.push_back({key, String(h["name"] | ""), h["hidden"] | false, String(h["color"] | "")});
  }
  for (auto &m : cfg.horns) {  // keep anything the page didn't send
    bool sent = false;
    for (auto &o : ordered) sent |= o.key == m.key;
    if (!sent) ordered.push_back(m);
  }
  cfg.horns = ordered;
  cfg.saveHorns();
  rebuildHorns();
}

void webSaveSettings(const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  if (doc["volume"].is<int>()) {
    cfg.volumePct = constrain(doc["volume"].as<int>(), 0, 100);
    cfg.saveVolume();
    applyVolume();
  }
  if (doc["brightness"].is<int>()) {
    cfg.brightnessPct = constrain(doc["brightness"].as<int>(), 10, 100);
    if (doc["save"] | false) cfg.saveBrightness();
    setBacklight(backlightLevel());
    dimmed = false;
    lastInput = millis();
  }
  if (doc["leds"].is<bool>()) {
    cfg.leds = doc["leds"];
    cfg.saveLeds();
  }
  if (doc["tint"].is<int>()) {
    cfg.tint = constrain(doc["tint"].as<int>(), 0, 2);
    cfg.saveTint();
  }
  if (doc["dimAfter"].is<int>() || doc["dimPct"].is<int>()) {
    if (doc["dimAfter"].is<int>()) cfg.dimAfterSec = max(0, doc["dimAfter"].as<int>());
    if (doc["dimPct"].is<int>()) cfg.dimPct = constrain(doc["dimPct"].as<int>(), 10, 100);
    cfg.saveDim();
    lastInput = millis();
    if (dimmed) { dimmed = false; setBacklight(backlightLevel()); }
  }
}

void webPlay(const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  String key = doc["key"] | "";
  for (size_t i = 0; i < horns.size(); i++)
    if (horns[i].key == key) { playHorn(i); return; }
  int track = trackOf(key);  // hidden horn: play it without selecting it
  if (track >= 0 && !demoMode) { playingHorn = -1; fx.play(track); }
}

// Start a transmission check if the car is ready for one; returns what to tell the driver.
static String startTransCheck() {
  ObdData d = obd.snapshot();
  if (d.state != ObdData::LIVE) return "Turn the ignition on first";
  if (!isnan(d.mph) && d.mph > 1) return "Stop the car first";
  obd.readTransmission();
  return "Checking transmission";
}

void webObd(const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  if (doc["resetTrip"] | false) { obd.resetTrip(); tripResetAt = millis(); }
  if (doc["fillUp"].is<float>() && doc["fillUp"].as<float>() > 0) obd.filledUp(doc["fillUp"].as<float>());
  if (doc["clearCodes"] | false) obd.clearCodes();
  if (doc["readTrans"] | false) startTransCheck();
  if (doc["sim"].is<bool>()) obd.setSimulate(doc["sim"]);
  if (doc["disp"].is<float>() && doc["disp"].as<float>() > 0) obd.setDisplacement(doc["disp"]);
  if (doc["tankCap"].is<float>() && doc["tankCap"].as<float>() > 0) obd.setTankCapacity(doc["tankCap"]);
  if (doc["forget"] | false) obd.forgetAdapter();
  if (doc["resetCal"] | false) obd.resetCalibration();
  if (doc["readOnly"].is<bool>()) obd.setReadOnly(doc["readOnly"]);
  if (doc["tankComplete"] | false) obd.markTankComplete();
  if (doc["fuelLevel"].is<float>()) obd.setFuelLevel(doc["fuelLevel"]);
  if (doc["relearnHealth"] | false) health.relearn();
}

void webMaint(const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  float tracked = obd.snapshot().odoMiles;
  if (doc["odometer"].is<float>() && doc["odometer"].as<float>() > 0) setOdometerReading(doc["odometer"]);
  if (doc["speedCal"].is<float>()) setSpeedCorrectionPct(constrain(doc["speedCal"].as<float>(), -15.0f, 15.0f));
  if (doc["done"].is<int>()) maint.markDone(doc["done"], tracked);
  if (doc["items"].is<JsonArray>()) {
    std::vector<ServiceItem> items;
    for (JsonObject o : doc["items"].as<JsonArray>()) {
      ServiceItem it{String(o["name"] | "Service"), (uint32_t)max(100, o["interval"] | 5000)};
      // "last" is an odometer reading; store it as tracked miles.
      if (o["last"].is<float>() && maint.odometerKnown()) it.lastTracked = o["last"].as<float>() - maint.odometer(0);
      items.push_back(it);
    }
    maint.items = items;
    maint.save();
  }
}

// History records tracked miles; add the odometer offset so charts can show
// real odometer readings once it has been entered.
String webHistoryJson() {
  String j = hist.json();
  if (maint.odometerKnown()) j = "{\"odoOffset\":" + String(maint.odometer(0), 1) + "," + j.substring(1);
  return j;
}

void webClearHistory() { hist.clear(); }

static const char *healthStatusKey(Health::Status s) {
  static const char *K[] = {"", "nodata", "learning", "ok", "watch", "check"};
  return K[s];
}

String webHealthJson() {
  JsonDocument doc;
  doc["drives"] = health.drives();
  doc["learnLeft"] = health.learningLeft();
  JsonArray arr = doc["checks"].to<JsonArray>();
  std::vector<Health::Result> r = health.results();
  for (int i = 0; i < (int)r.size(); i++) {
    if (r[i].status == Health::NOT_APPLICABLE) continue;
    JsonObject o = arr.add<JsonObject>();
    o["name"] = Health::checkName(i);
    o["status"] = healthStatusKey(r[i].status);
    o["summary"] = r[i].summary;
    o["detail"] = r[i].detail;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String webBackupJson() {
  events.add("Backup downloaded");
  return Backup::make(FW_VERSION, [](const char *name) { return strcmp(name, "health") ? hist.rawFile(name) : health.raw(); });
}

// Check and keep an uploaded backup; it's applied after the restart that follows.
String webRestore(const String &body) { return Backup::stage(body); }

// Erase the file store and write back what's in memory (drive history, engine
// health). The event log starts again. Refused while the demo car is on, when
// the real history isn't in memory.
static bool repairStorage() {
  if (hist.demoOn()) return false;
  if (!Storage::reformat()) return false;
  bool ok = hist.resave();
  health.resave();
  events.add("Storage repaired: %d drives written back", (int)hist.driveCount());
  return ok;
}

String webLogText() {
  return "Sorento Dash " FW_VERSION ", restart #" + String(events.bootNumber()) + ", up " + String(millis() / 60000) +
         " min\n" + events.text();
}

void webClearLog() {
  events.clear();
  events.add("Event log cleared");
}

// Firmware upload progress, drawn straight away (the upload blocks the loop).
static void render(uint32_t now);
void webOtaProgress(size_t done, size_t total) {
  int pct = total ? min<int>(100, done * 100 / total) : 0;
  if (mode != MODE_OTA) { mode = MODE_OTA; setBacklight(backlightLevel()); }
  if (pct == otaPct && done > 0) return;
  otaPct = pct;
  render(millis());
}

void webOtaFinished(bool ok) {
  otaOk = ok;
  if (ok) events.add("Firmware update received, restarting");
  if (ok) {
    otaPct = 100;
    render(millis());
  } else {
    mode = MODE_WIFI;
    showToast("Update failed");
  }
}

// ---------------------------------------------------------------- LEDs
static void setRing(uint32_t c, float b) {
  for (int i = 0; i < LED_RING_COUNT; i++)
    ring.setPixelColor(i, (uint8_t)(((c >> 16) & 0xFF) * b), (uint8_t)(((c >> 8) & 0xFF) * b), (uint8_t)((c & 0xFF) * b));
}

static void updateLeds(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 20) return;
  last = now;
  ring.clear();
  if (cfg.leds && isPlaying()) {
    uint32_t c = playingHorn >= 0 ? accentFor(playingHorn) : C_TEXT;
    float t = now / 90.0f;
    for (int i = 0; i < LED_RING_COUNT; i++) {
      float b = 0.25f + 0.75f * powf(0.5f + 0.5f * sinf(t - i * 1.2f), 3);
      ring.setPixelColor(i, (uint8_t)(((c >> 16) & 0xFF) * b), (uint8_t)(((c >> 8) & 0xFF) * b), (uint8_t)((c & 0xFF) * b));
    }
  } else if (cfg.leds && carAlert(car)) {
    setRing(C_BAD, 0.1f + 0.35f * (0.5f + 0.5f * sinf(now / 300.0f)));  // slow red pulse
  } else if (cfg.leds && now - selChangedAt < 900 && !horns.empty() && mode == MODE_HORNS) {
    // Brief glow in the horn's colour after turning the knob.
    setRing(accentFor(selected()), 0.35f * (1.0f - (now - selChangedAt) / 900.0f));
  }
  ring.show();
}

// ---------------------------------------------------------------- drawing
constexpr int CX = 120, CY = 120;

// Antialiased Inter fonts (see tools/make_fonts.py).
lgfx::PointerWrapper wTitle(FONT_TITLE), wBody(FONT_BODY), wSmall(FONT_SMALL), wHuge(FONT_HUGE);
lgfx::VLWfont fTitle, fBody, fSmall, fHuge;

static void loadFonts() {
  fTitle.loadFont(&wTitle);
  fBody.loadFont(&wBody);
  fSmall.loadFont(&wSmall);
  fHuge.loadFont(&wHuge);
}

static void text(const String &s, int x, int y, const lgfx::IFont *font, uint32_t color) {
  canvas.setFont(font);
  canvas.setTextColor(rgb(color));
  canvas.setTextDatum(middle_center);
  canvas.drawString(s, x, y);
}

// Arc with round end caps. Angles in degrees, 0 = 3 o'clock, clockwise.
static void arc(int rIn, int rOut, float a0, float a1, uint32_t color) {
  if (a1 - a0 < 0.5f) return;
  uint16_t c = rgb(color);
  canvas.fillArc(CX, CY, rOut, rIn, a0, a1, c);
  float rm = (rIn + rOut) / 2.0f, rc = (rOut - rIn) / 2.0f;
  for (float a : {a0, a1}) {
    float r = a * DEG_TO_RAD;
    canvas.fillSmoothCircle(CX + cosf(r) * rm, CY + sinf(r) * rm, rc, c);
  }
}

// Widest text that fits across the circle at height y.
static int chordWidth(int y, int margin) {
  int dy = y - CY;
  return 2 * (int)sqrtf(max(0, 118 * 118 - dy * dy)) - margin;
}

constexpr int RING_IN = 110, RING_OUT = 117;

// Outer ring with one segment per item and the (animated) selection lit.
static void drawSegmentRing(int n, float pos, uint32_t accent, uint32_t track) {
  float seg = 360.0f / n;
  float gap = n > 1 ? min(4.0f, seg * 0.3f) : 0;
  if (n <= 36) {
    for (int i = 0; i < n; i++) {
      float mid = -90 + seg * i;
      canvas.fillArc(CX, CY, RING_OUT, RING_IN, mid - seg / 2 + gap / 2, mid + seg / 2 - gap / 2, rgb(track));
    }
  } else {
    canvas.fillArc(CX, CY, RING_OUT, RING_IN, 0, 360, rgb(track));
  }
  float p = fmodf(fmodf(pos, n) + n, n);
  float mid = -90 + seg * p;
  float half = max(seg / 2 - gap / 2, 5.0f);
  arc(RING_IN, RING_OUT, mid - half, mid + half, accent);
}

// Vertical drum of labels centred on the wheel position.
static void drawDrum(const Wheel &w, int n, std::function<String(int)> label, std::function<uint32_t(int)> centreColor,
                     uint32_t dimColor, uint32_t bg) {
  constexpr int SPACING = 54;
  canvas.setClipRect(0, 52, 240, 140);
  int base = floorf(w.pos);
  for (int k = base - 1; k <= base + 2; k++) {
    float d = k - w.pos;
    float ad = fabsf(d);
    if (ad > 1.6f) continue;
    if (n == 1 && k != base) continue;
    int idx = ((k % n) + n) % n;
    int y = CY + d * SPACING;
    bool centre = ad < 0.5f;
    String s = label(idx);
    const lgfx::IFont *f = centre ? (const lgfx::IFont *)&fTitle : (const lgfx::IFont *)&fBody;
    canvas.setFont(f);
    if (centre && canvas.textWidth(s) > chordWidth(y, 36)) f = &fBody;
    uint32_t col = centre ? centreColor(idx) : lerpHex(dimColor, bg, (ad - 0.5f) / 1.1f);
    text(s, CX, y, f, col);
  }
  canvas.clearClipRect();
}

static void drawEqualizer(int y, uint32_t color, uint32_t now) {
  constexpr int BARS = 5, W = 4, GAP = 4;
  int x0 = CX - (BARS * W + (BARS - 1) * GAP) / 2;
  float t = now / 1000.0f;
  for (int i = 0; i < BARS; i++) {
    float h = 4 + 12 * (0.5f + 0.5f * sinf(t * (7 + i * 1.7f) + i * 2.1f)) * (0.6f + 0.4f * sinf(t * 3.1f + i));
    canvas.fillRoundRect(x0 + i * (W + GAP), y - (int)h, W, (int)h, 2, rgb(color));
  }
}

// Background for the horn screen: black, or tinted with the horn's colour.
static uint32_t hornBackground(bool playing) {
  if (cfg.tint == 0 || horns.empty()) return 0;
  uint32_t c;
  if (playing && playingHorn >= 0) {
    c = accentFor(playingHorn);
  } else {  // blend between neighbours while the drum scrolls
    int a = floorf(hornWheel.pos);
    float f = hornWheel.pos - a;
    int n = horns.size();
    c = lerpHex(horns[((a % n) + n) % n].color, horns[(((a + 1) % n) + n) % n].color, f);
  }
  return lerpHex(0, c, cfg.tint == 1 ? 0.16f : 0.42f);
}

static void drawHornScreen(uint32_t now) {
  int n = horns.size();
  bool playing = isPlaying();
  uint32_t bg = hornBackground(playing);
  uint32_t track = lerpHex(bg, 0xFFFFFF, 0.09f);
  canvas.fillScreen(rgb(bg));

  if (n == 0) {
    arc(RING_IN, RING_OUT, 0, 360, track);
    text("No horns to show", CX, CY - 10, &fBody, C_TEXT);
    text("Menu > Phone setup", CX, CY + 14, &fSmall, C_DIM);
    return;
  }

  int active = playing && playingHorn >= 0 ? playingHorn : selected();
  uint32_t accent = accentFor(active);

  if (playing) {
    canvas.fillArc(CX, CY, RING_OUT, RING_IN, 0, 360, rgb(track));
    float p = playProgress();
    if (p >= 0) {
      arc(RING_IN, RING_OUT, -90, -90 + 360 * max(p, 0.02f), accent);
    } else {  // length unknown: spinning segment
      float a = fmodf(now * 0.36f, 360);
      arc(RING_IN, RING_OUT, a, a + 60, accent);
    }
  } else {
    drawSegmentRing(n, hornWheel.pos, accent, track);
  }

  // Top: equalizer while playing, otherwise any status badge.
  const char *alert = carAlert(car);
  if (playing) drawEqualizer(46, accent, now);
  else if (alert) text(alert, CX, 44, &fSmall, C_BAD);
  else if (demoMode) text("DEMO", CX, 44, &fSmall, C_WARN);
  else if (!fx.connected()) text("NO SOUND BOARD", CX, 44, &fSmall, C_WARN);
  else if (serviceDue() >= 0) {
    const ServiceItem &it = maint.items[serviceDue()];
    String b = it.name + (maint.remaining(it, car.odoMiles) < 0 ? " OVERDUE" : " DUE");
    b.toUpperCase();
    canvas.setFont(&fSmall);
    text(canvas.textWidth(b) <= chordWidth(44, 40) ? b : String("SERVICE DUE"), CX, 44, &fSmall, C_WARN);
  } else if (web.active()) text("PHONE SETUP ON", CX, 44, &fSmall, C_WIFI);

  uint32_t dim = lerpHex(playing ? C_TICK : C_DIM, bg, 0.2f);
  drawDrum(
      hornWheel, n, [](int i) { return horns[i].label; },
      [&](int i) { return playing && i == playingHorn && cfg.tint < 2 ? accent : C_TEXT; }, dim, bg);

  if (playing) canvas.fillRoundRect(CX - 7, 196, 14, 14, 3, rgb(cfg.tint == 2 ? C_TEXT : accent));
}

static void drawMenuScreen() {
  if (!menu.open()) return;
  Menu::Level &l = menu.top();
  int n = l.items.size();
  drawSegmentRing(n, l.wheel.pos, C_MENU, C_TRACK);
  String crumb = menu.breadcrumb();
  canvas.setFont(&fSmall);
  if (canvas.textWidth(crumb) > chordWidth(44, 40)) {  // too long: just this level
    crumb = l.title;
    crumb.toUpperCase();
  }
  text(crumb, CX, 44, &fSmall, C_DIM);
  drawDrum(l.wheel, n, [&l](int i) { return l.items[i].label; }, [](int) { return C_TEXT; }, C_DIM, 0);
  if (fabsf(l.wheel.target - l.wheel.pos) < 0.3f) {
    MenuItem &it = l.items[l.wheel.index(n)];
    String v = it.value ? it.value() : String();
    if (it.kind == MenuItem::SUB && !v.length()) v = "open";
    if (v.length()) text(v, CX, CY + 26, &fSmall, it.valueColor ? it.valueColor() : C_DIM);
  }
}

// 270-degree gauge (gap at the bottom). frac 0..1.
constexpr float G_A0 = 135, G_SWEEP = 270;
static void drawGaugeArc(float frac, uint32_t color) {
  arc(100, 112, G_A0, G_A0 + G_SWEEP, C_TRACK);
  if (frac > 0.005f) arc(100, 112, G_A0, G_A0 + G_SWEEP * constrain(frac, 0.0f, 1.0f), color);
}

static void drawGauge(const char *title, int pct, uint32_t color) {
  drawGaugeArc(pct / 100.0f, color);
  text(title, CX, CY - 42, &fSmall, C_DIM);
  text(String(pct), CX, CY + 4, &fHuge, C_TEXT);
  text("%", CX, CY + 46, &fBody, C_DIM);
}

static void drawWifiScreen() {
  int st = web.stations();
  String qr = st ? String(web.url) : web.joinUri();
  constexpr int QR = 124, Q0 = CX - QR / 2, QY = 62;
  canvas.fillRoundRect(Q0 - 8, QY - 8, QR + 16, QR + 16, 10, TFT_WHITE);
  canvas.qrcode(qr.c_str(), Q0, QY, QR);
  if (st) {
    text("Open in browser", CX, 40, &fSmall, C_WIFI);
    text("192.168.4.1", CX, 206, &fSmall, C_TEXT);
  } else {
    text("Scan to join Wi-Fi", CX, 40, &fSmall, C_WIFI);
    text(web.ssid, CX, 200, &fSmall, C_TEXT);
    text("pw " + web.password, CX, 217, &fSmall, C_DIM);
  }
}

// ---- car pages
static String fmt(float v, int decimals) { return isnan(v) ? String("--") : String(v, decimals); }

static void drawPageDots() {
  constexpr int GAP = 12;
  int x0 = CX - (CP_COUNT - 1) * GAP / 2;
  for (int i = 0; i < CP_COUNT; i++)
    canvas.fillSmoothCircle(x0 + i * GAP, 222, i == carPage ? 3 : 2, rgb(i == carPage ? C_TEXT : C_TICK));
}

// Two stats side by side near the bottom of a gauge page.
static void drawStatPair(const String &a, const char *la, const String &b, const char *lb, int y) {
  text(a, CX - 40, y, &fBody, C_TEXT);
  text(b, CX + 40, y, &fBody, C_TEXT);
  text(la, CX - 40, y + 18, &fSmall, C_DIM);
  text(lb, CX + 40, y + 18, &fSmall, C_DIM);
}

// Shown on every car page until data flows.
static bool drawCarStatus(const char *title) {
  // Only while actually disconnected: the slow K-line can leave a reading a
  // few seconds old, which is no reason to cover the page.
  if (car.state == ObdData::LIVE) return false;
  drawGaugeArc(0, 0);
  text(title, CX, 56, &fSmall, C_DIM);
  text(car.stateText(), CX, CY - 4, &fBody, car.state == ObdData::NOT_FOUND ? C_WARN : C_TEXT);
  const char *hint = "";
  switch (car.state) {
    case ObdData::NOT_FOUND: hint = "Plug in a BLE OBD adapter"; break;
    case ObdData::SEARCHING: hint = "Scanning Bluetooth..."; break;
    case ObdData::NO_ECU: hint = "Turn the key to ON"; break;
    default: hint = car.adapter; break;
  }
  text(hint, CX, CY + 20, &fSmall, C_DIM);
  return true;
}

static uint32_t mpgColor(float mpg) { return mpg < 12 ? C_BAD : mpg < 18 ? C_WARN : C_GOOD; }

static void drawEconomyPage() {
  if (drawCarStatus("ECONOMY")) return;
  bool moving = !isnan(car.instMpg);
  if (moving) {
    drawGaugeArc(car.instMpg / 40.0f, mpgColor(car.instMpg));
    text("INSTANT", CX, 56, &fSmall, C_DIM);
    text(String(car.instMpg, car.instMpg < 10 ? 1 : 0), CX, CY - 2, &fHuge, C_TEXT);
    text("MPG", CX, CY + 38, &fSmall, C_DIM);
  } else {
    drawGaugeArc(0, 0);
    text("IDLE", CX, 56, &fSmall, C_DIM);
    text(fmt(car.gph, 1), CX, CY - 2, &fHuge, C_TEXT);
    text("GAL/H", CX, CY + 38, &fSmall, C_DIM);
  }
  float avg = car.drive.mpg();  // this drive; the trip meter's average is on the trip page
  text(avg > 0 ? String(avg, 1) + " drive avg" : String("-- drive avg"), CX, 186, &fBody, avg > 0 ? mpgColor(avg) : C_DIM);
}

static void drawTripPage() {
  if (drawCarStatus("TRIP")) return;
  float range = car.rangeMiles();
  text("RANGE", CX, 56, &fSmall, C_DIM);
  if (isnan(range)) {
    drawGaugeArc(0, 0);
    text("--", CX, 94, &fHuge, C_DIM);
    text("press to log a fill-up", CX, 128, &fSmall, C_DIM);
  } else {
    float frac = car.tankFraction();
    drawGaugeArc(frac, frac > 0.25f ? C_GOOD : frac > 0.12f ? C_WARN : C_BAD);
    text(String(range, 0), CX, 94, &fHuge, C_TEXT);
    text("miles left", CX, 128, &fSmall, C_DIM);
  }
  float avg = car.trip.mpg();
  drawStatPair(String(car.trip.miles, car.trip.miles < 100 ? 1 : 0), "TRIP MI", avg > 0 ? String(avg, 1) : String("--"),
               "AVG MPG", 158);
}

static void drawEnginePage() {
  if (drawCarStatus("ENGINE")) return;
  float c = car.coolantF;
  uint32_t col = isnan(c) ? C_DIM : c < 150 ? C_COLD : c < 230 ? C_GOOD : C_BAD;
  drawGaugeArc(isnan(c) ? 0 : (c - 100) / 160.0f, col);
  text("COOLANT", CX, 54, &fSmall, C_DIM);
  text(isnan(c) ? String("--") : String(c, 0) + "\xC2\xB0", CX + 8, 94, &fHuge, C_TEXT);
  uint32_t vcol = carAlert(car) && !isnan(car.volts) && (car.volts < 12.6f || car.volts > 15.3f) ? C_BAD : C_TEXT;
  text(fmt(car.volts, 1), CX - 56, 150, &fBody, vcol);
  text(fmt(car.rpm, 0), CX, 150, &fBody, C_TEXT);
  text(isnan(car.intakeF) ? String("--") : String(car.intakeF, 0) + "\xC2\xB0", CX + 56, 150, &fBody, C_TEXT);
  text("VOLTS", CX - 56, 168, &fSmall, C_DIM);
  text("RPM", CX, 168, &fSmall, C_DIM);
  text("INTAKE", CX + 56, 168, &fSmall, C_DIM);
}

// Word-wrap into at most two lines that fit `width`.
static void wrapText(const String &s, int x, int y, int width, uint32_t color) {
  canvas.setFont(&fSmall);
  if (canvas.textWidth(s) <= width) { text(s, x, y, &fSmall, color); return; }
  int cut = s.length();
  while (cut > 0) {
    cut = s.lastIndexOf(' ', cut - 1);
    if (cut <= 0) break;
    if (canvas.textWidth(s.substring(0, cut)) <= width) break;
  }
  if (cut <= 0) { text(s, x, y, &fSmall, color); return; }
  text(s.substring(0, cut), x, y - 8, &fSmall, color);
  text(s.substring(cut + 1), x, y + 8, &fSmall, color);
}

// The engine computer's codes plus any read from the transmission computer.
static std::vector<uint16_t> allCodes() {
  std::vector<uint16_t> v(car.dtcs, car.dtcs + car.nDtcs);
  for (int i = 0; i < car.nTransDtcs; i++)
    if (std::find(v.begin(), v.end(), car.transDtcs[i]) == v.end()) v.push_back(car.transDtcs[i]);
  return v;
}

static void drawCodesPage() {
  if (drawCarStatus("TROUBLE CODES")) return;
  std::vector<uint16_t> codes = allCodes();
  int nCodes = codes.size();
  uint32_t col = nCodes ? C_WARN : C_GOOD;
  arc(RING_IN, RING_OUT, 0, 360, lerpHex(0, col, 0.45f));
  text("TROUBLE CODES", CX, 44, &fSmall, C_DIM);
  if (!car.dtcsRead) {
    text("Reading...", CX, CY, &fBody, C_TEXT);
    return;
  }
  if (nCodes == 0) {
    // Check mark.
    for (int w = -3; w <= 3; w++) {
      canvas.drawLine(CX - 22, CY - 16 + w, CX - 6, CY + 2 + w, rgb(C_GOOD));
      canvas.drawLine(CX - 6, CY + 2 + w, CX + 24, CY - 30 + w, rgb(C_GOOD));
    }
    text("No trouble codes", CX, CY + 42, &fBody, C_TEXT);
    if (car.milOn) text("light on, no stored code", CX, CY + 64, &fSmall, C_DIM);
    return;
  }
  int n = min(nCodes, 3);
  int spacing = n == 1 ? 0 : 50;
  int y0 = CY - (n - 1) * spacing / 2 - (n == 1 ? 12 : 8);
  for (int i = 0; i < n; i++) {
    String code = ObdData::dtcText(codes[i]);
    const char *desc = dtcDescription(code);
    int y = y0 + i * spacing;
    text(code, CX, y, n == 1 ? &fTitle : &fBody, C_WARN);
    if (n == 1) wrapText(desc ? desc : "Look this code up online", CX, y + 36, 170, C_TEXT);
    else text(desc ? desc : "", CX, y + 20, &fSmall, C_DIM);
  }
  if (nCodes > 3) text("+" + String(nCodes - 3) + " more on phone", CX, 196, &fSmall, C_DIM);
}

// ---- engine health
static uint32_t healthColor(Health::Status s) {
  return s == Health::CHECK ? C_BAD : s == Health::WATCH ? C_WARN : s == Health::OK ? C_GOOD : C_DIM;
}

// Refreshed once a second for the page and menus.
static Health::Overview healthOverview() {
  static Health::Overview o;
  static uint32_t at = 0;
  if (!at || millis() - at >= 1000) {
    at = millis();
    o = health.overview();
  }
  return o;
}
static String healthSummaryText() {
  Health::Overview o = healthOverview();
  if (o.check) return String(o.check) + " to check";
  if (o.watch) return String(o.watch) + " to watch";
  if (o.ok) return "all good";
  return "learning";
}

static void drawHealthPage() {
  Health::Overview o = healthOverview();
  uint32_t col = o.check ? C_BAD : o.watch ? C_WARN : o.ok ? C_GOOD : C_DIM;
  arc(RING_IN, RING_OUT, 0, 360, lerpHex(0, col, 0.45f));
  text("ENGINE HEALTH", CX, 44, &fSmall, C_DIM);
  if (o.check || o.watch) {
    text(String(o.check ? o.check : o.watch) + (o.check ? " to check" : " to watch"), CX, CY - 30, &fTitle, col);
    text(Health::checkName(o.worst), CX, CY + 6, &fBody, C_TEXT);
    text(o.worstSummary, CX, CY + 30, &fSmall, C_DIM);
    if (o.check && o.watch) text("+" + String(o.watch) + " to watch", CX, CY + 50, &fSmall, C_WARN);
  } else if (o.ok) {
    for (int w = -3; w <= 3; w++) {  // check mark
      canvas.drawLine(CX - 22, CY - 16 + w, CX - 6, CY + 2 + w, rgb(C_GOOD));
      canvas.drawLine(CX - 6, CY + 2 + w, CX + 24, CY - 30 + w, rgb(C_GOOD));
    }
    text("All good", CX, CY + 42, &fBody, C_TEXT);
    int left = health.learningLeft();
    text(left ? "learning: " + String(left) + " drives to go" : String(o.ok) + " checks OK", CX, CY + 64, &fSmall, C_DIM);
  } else {
    text("Learning", CX, CY - 6, &fTitle, C_TEXT);
    text("drive to collect data", CX, CY + 24, &fSmall, C_DIM);
  }
}

static uint32_t serviceColor(float r) { return r < 0 ? C_BAD : r <= Maintenance::DUE_SOON_MI ? C_WARN : C_GOOD; }

static String withCommas(float v) {
  char b[16];
  snprintf(b, sizeof(b), "%ld", lroundf(v));
  String s(b), out;
  int n = s.length();
  for (int i = 0; i < n; i++) {
    out += s[i];
    if ((n - i - 1) % 3 == 0 && i < n - 1 && s[i] != '-') out += ',';
  }
  return out;
}

static void drawServicePage() {
  float tracked = car.odoMiles;
  int u = maint.mostUrgent(tracked);
  if (u < 0) {
    drawGaugeArc(0, 0);
    text("SERVICE", CX, 56, &fSmall, C_DIM);
    text("Nothing logged yet", CX, CY - 6, &fBody, C_TEXT);
    text("Press to log a service", CX, CY + 18, &fSmall, C_DIM);
  } else {
    const ServiceItem &it = maint.items[u];
    float r = maint.remaining(it, tracked);
    drawGaugeArc(r / it.interval, serviceColor(r));
    text("NEXT SERVICE", CX, 50, &fSmall, C_DIM);
    text(it.name, CX, 74, &fBody, C_TEXT);
    text(withCommas(fabsf(r)), CX, 112, &fHuge, serviceColor(r) == C_GOOD ? C_TEXT : serviceColor(r));
    text(r < 0 ? "miles overdue" : "miles left", CX, 146, &fSmall, C_DIM);
  }
  text(maint.odometerKnown() ? withCommas(maint.odometer(tracked)) + " mi" : String("Set odometer on phone"), CX, 176,
       &fSmall, C_DIM);
}

static void drawFillupScreen() {
  float cap = car.tankCapacity;
  drawGaugeArc(fillGallons / cap, C_VOLUME);
  text("FILL-UP", CX, 56, &fSmall, C_DIM);
  text(String(fillGallons, 1), CX, CY - 2, &fHuge, C_TEXT);
  text("gallons pumped", CX, CY + 36, &fSmall, C_DIM);
  text("press to save", CX, 176, &fSmall, C_GOOD);
  text("hold to cancel", CX, 194, &fSmall, C_DIM);
}

static String serviceStatus(const ServiceItem &it) {
  float r = maint.remaining(it, car.odoMiles);
  if (isnan(r)) return "not logged yet";
  return r < 0 ? String(-r, 0) + " mi overdue" : "due in " + String(r, 0) + " mi";
}

static String numberText(float v, int decimals) {
  return decimals == 0 && fabsf(v) >= 10000 ? withCommas(v) : String(v, decimals);
}

static void drawNumberScreen() {
  float range = editItem.max - editItem.min;
  drawGaugeArc(range > 0 ? (editValue - editItem.min) / range : 0, C_WIFI);
  String title = editItem.label;
  title.toUpperCase();
  text(title, CX, 56, &fSmall, C_DIM);
  String v = numberText(editValue, editItem.decimals);
  canvas.setFont(&fHuge);
  text(v, CX, CY - 2, canvas.textWidth(v) <= 180 ? &fHuge : &fTitle, C_TEXT);
  if (*editItem.unit) text(editItem.unit, CX, CY + 36, &fSmall, C_DIM);
  text("press to save", CX, 176, &fSmall, C_GOOD);
  text("hold to cancel", CX, 194, &fSmall, C_DIM);
}

static void drawConfirmScreen() {
  arc(RING_IN, RING_OUT, 0, 360, lerpHex(0, C_WARN, 0.45f));
  String q = confirmQuestion;
  q.toUpperCase();
  text(q, CX, 56, &fSmall, C_DIM);
  canvas.setFont(&fTitle);
  text(confirmSubject, CX, CY - 6, canvas.textWidth(confirmSubject) <= chordWidth(CY - 6, 36) ? &fTitle : &fBody, C_TEXT);
  text("press to confirm", CX, 170, &fSmall, C_GOOD);
  text("hold to cancel", CX, 188, &fSmall, C_DIM);
}

// Text screen: lines are wrapped once, narrow enough for the lowest row of the round screen.
constexpr int TEXT_Y0 = 66, TEXT_LH = 16, TEXT_ROWS = 8, TEXT_W = 160;

static void openText(const String &title, const String &body) {
  textTitle = title;
  textTitle.toUpperCase();
  textLines.clear();
  textScroll = 0;
  canvas.setFont(&fSmall);
  int start = 0;
  while (start <= (int)body.length()) {  // paragraph by paragraph
    int nl = body.indexOf('\n', start);
    String para = body.substring(start, nl < 0 ? body.length() : nl);
    start = nl < 0 ? body.length() + 1 : nl + 1;
    String line;
    int i = 0;
    while (i < (int)para.length()) {
      int sp = para.indexOf(' ', i);
      String word = para.substring(i, sp < 0 ? para.length() : sp);
      i = sp < 0 ? para.length() : sp + 1;
      String tryLine = line.length() ? line + " " + word : word;
      if (line.length() && canvas.textWidth(tryLine) > TEXT_W) {
        textLines.push_back(line);
        line = word;
      } else {
        line = tryLine;
      }
    }
    textLines.push_back(line);  // empty for a blank line between paragraphs
  }
  mode = MODE_TEXT;
}

static int textMaxScroll() { return max(0, (int)textLines.size() - TEXT_ROWS); }

static void drawTextScreen() {
  text(textTitle, CX, 44, &fSmall, C_DIM);
  for (int r = 0; r < TEXT_ROWS && textScroll + r < (int)textLines.size(); r++)
    text(textLines[textScroll + r], CX, TEXT_Y0 + r * TEXT_LH, &fSmall, C_TEXT);
  int maxS = textMaxScroll();
  if (maxS > 0) {  // scroll position on the right edge
    constexpr float A0 = -32, A1 = 32;
    float span = (A1 - A0) * TEXT_ROWS / textLines.size();
    float at = A0 + (A1 - A0 - span) * textScroll / maxS;
    arc(RING_IN, RING_OUT, A0, A1, C_TRACK);
    arc(RING_IN, RING_OUT, at, at + span, C_MENU);
  }
  text(textScroll < maxS ? "turn for more" : "press to go back", CX, 204, &fSmall, C_DIM);
}

static void drawOtaScreen() {
  arc(RING_IN, RING_OUT, 0, 360, C_TRACK);
  if (otaPct > 0) arc(RING_IN, RING_OUT, -90, -90 + 3.6f * otaPct, C_WIFI);
  text(otaOk ? "RESTARTING" : "UPDATING", CX, 56, &fSmall, C_WIFI);
  text(String(otaPct), CX, CY - 2, &fHuge, C_TEXT);
  text("%", CX, CY + 36, &fBody, C_DIM);
  text("keep power on", CX, 184, &fSmall, C_DIM);
}

static void drawToast() {
  if (!toastText.length() || millis() > toastUntil) return;
  canvas.setFont(&fSmall);
  int w = canvas.textWidth(toastText) + 28;
  canvas.fillRoundRect(CX - w / 2, 172, w, 28, 14, rgb(0x2A2E35));
  text(toastText, CX, 186, &fSmall, C_TEXT);
}

// ---- details (press on Economy / Engine)
static uint32_t trimColor(float v) {
  float a = fabsf(v);
  return isnan(v) ? C_DIM : a <= 5 ? C_GOOD : a <= 10 ? C_WARN : C_BAD;
}
static String pct(float v) { return isnan(v) ? String("--") : (v >= 0 ? "+" : "") + String(v, 1) + "%"; }

static void drawEngineDetail() {
  text("FUEL TRIMS", CX, 40, &fSmall, C_DIM);
  if (car.state != ObdData::LIVE) {
    text(car.stateText(), CX, CY, &fBody, C_TEXT);
    return;
  }
  bool two = !isnan(car.ltft[1]) || !isnan(car.stft[1]);
  for (int bank = 0; bank < (two ? 2 : 1); bank++) {
    int x = two ? CX + (bank ? 52 : -52) : CX;
    text(two ? (bank ? "BANK 2" : "BANK 1") : "BANK 1", x, 64, &fSmall, C_DIM);
    text(pct(car.ltft[bank]), x, 92, &fTitle, trimColor(car.ltft[bank]));
    text("long term", x, 114, &fSmall, C_DIM);
    text(pct(car.stft[bank]), x, 140, &fBody, trimColor(car.stft[bank]));
    text("short term", x, 158, &fSmall, C_DIM);
  }
  // Long-term trims tell the story: the computer's learned correction.
  float lt = two ? (car.ltft[0] + car.ltft[1]) / 2 : car.ltft[0];
  const char *verdict = isnan(lt) ? "trims not reported" : lt > 10 ? "running lean - vacuum leak?" :
                        lt < -10 ? "running rich" : lt > 5 || lt < -5 ? "slightly off, keep an eye on it" : "fuel mixture normal";
  text(verdict, CX, 182, &fSmall, isnan(lt) ? C_DIM : trimColor(lt));
  String extra = (isnan(car.timingDeg) ? String("") : "timing " + String(car.timingDeg, 0) + "\xC2\xB0  ") +
                 (isnan(car.loadPct) ? String("") : "load " + String(car.loadPct, 0) + "%");
  if (extra.length()) text(extra, CX, 202, &fSmall, C_DIM);
}

static void drawEconomyDetail() {
  text("FUEL DETAILS", CX, 40, &fSmall, C_DIM);
  if (car.state != ObdData::LIVE) {
    text(car.stateText(), CX, CY, &fBody, C_TEXT);
    return;
  }
  auto cell = [](int x, int y, const String &v, const char *label) {
    text(v, x, y, &fTitle, C_TEXT);
    text(label, x, y + 22, &fSmall, C_DIM);
  };
  cell(CX - 52, 82, fmt(car.gph, 2), "GAL / HOUR");
  cell(CX + 52, 82, fmt(car.mph, 0), "MPH");
  cell(CX - 52, 142, fmt(car.rpm, 0), "RPM");
  cell(CX + 52, 142, isnan(car.loadPct) ? String("--") : String(car.loadPct, 0) + "%", "LOAD");
  if (!isnan(car.throttlePct)) text("throttle " + String(car.throttlePct, 0) + "%", CX, 196, &fSmall, C_DIM);
}

static void drawDetailScreen() {
  if (carPage == CP_ENGINE) drawEngineDetail();
  else drawEconomyDetail();
  if (car.simulated) text("DEMO CAR", CX, 218, &fSmall, C_WARN);
}

// ---- history
static void drawHistoryPage() {
  History::Totals t = hist.totals();
  text("HISTORY", CX, 56, &fSmall, C_DIM);
  if (!t.drives) {
    drawGaugeArc(0, 0);
    text("No drives yet", CX, CY - 6, &fBody, C_TEXT);
    text("recorded with the OBD adapter", CX, CY + 18, &fSmall, C_DIM);
    return;
  }
  float mpg = t.gallons > 0.1f ? t.miles / t.gallons : NAN;
  drawGaugeArc(isnan(mpg) ? 0 : mpg / 40.0f, isnan(mpg) ? C_DIM : mpgColor(mpg));
  text(fmt(mpg, 1), CX, 94, &fHuge, C_TEXT);
  text("lifetime MPG", CX, 128, &fSmall, C_DIM);
  drawStatPair(String(t.drives), "DRIVES", withCommas(t.miles), "MILES", 154);
  text("press for charts", CX, 190, &fSmall, C_DIM);
}

// Plot box for the history charts (inside the round screen).
constexpr int PX0 = 38, PX1 = 202, PY0 = 76, PY1 = 166;

// Column with a 3px rounded top, square at the baseline.
static void column(int x, int w, float frac, uint32_t color) {
  int h = max(2, (int)(frac * (PY1 - PY0)));
  canvas.fillRoundRect(x, PY1 - h, w, h, min(3, w / 2), rgb(color));
  canvas.fillRect(x, PY1 - min(h, 3), w, min(h, 3), rgb(color));
}

static float roundUp(float v, float step) { return max(step, ceilf(v / step) * step); }

static void drawSpeedChart() {
  History::BandStats b = hist.bandStats();
  int idx[History::BANDS], n = 0;
  float mpg[History::BANDS], top = 0;
  int best = -1;
  for (int i = 0; i < History::BANDS; i++) {
    if (b.miles[i] < 0.5f || b.gallons[i] < 0.01f) continue;
    idx[n] = i;
    mpg[n] = b.miles[i] / b.gallons[i];
    if (best < 0 || mpg[n] > mpg[best]) best = n;
    top = max(top, mpg[n]);
    n++;
  }
  if (!n) {
    text("Not enough driving yet", CX, CY, &fSmall, C_DIM);
    return;
  }
  float ymax = roundUp(top * 1.1f, 5);
  float slot = (PX1 - PX0) / (float)n;
  int w = min(16, (int)slot - 4);
  canvas.drawFastHLine(PX0 - 4, PY1, PX1 - PX0 + 8, rgb(C_TICK));
  for (int k = 0; k < n; k++) {
    int x = PX0 + slot * k + (slot - w) / 2;
    column(x, w, mpg[k] / ymax, k == best ? C_GOOD : C_BLUE);
    int i = idx[k];
    String label = i == 0 ? "<" + String(History::BAND_MIN[1]) : String(History::BAND_MIN[i]) + (i == History::BANDS - 1 ? "+" : "");
    if (n <= 6 || k % 2 == 0 || k == best) text(label, x + w / 2, PY1 + 11, &fSmall, k == best ? C_TEXT : C_DIM);
  }
  // Only the winner gets a value on its cap; the caption tells the story.
  int bx = PX0 + slot * best + slot / 2;
  text(String(mpg[best], 1), bx, PY1 - (int)(mpg[best] / ymax * (PY1 - PY0)) - 10, &fSmall, C_TEXT);
  int i = idx[best];
  String range = i == History::BANDS - 1 ? String(History::BAND_MIN[i]) + "+" :
                 (i == 0 ? String("under ") + History::BAND_MIN[1] : String(History::BAND_MIN[i]) + "-" + String(History::BAND_MIN[i + 1] - 1));
  text("best at " + range + " mph", CX, 196, &fSmall, C_DIM);
}

static void drawRecentChart() {
  std::vector<DriveRec> all = hist.recentDrives(40);
  std::vector<DriveRec> d;
  for (auto &r : all)
    if (r.miles >= 0.5f && r.gallons > 0.05f) d.push_back(r);
  if (d.size() > 12) d.erase(d.begin(), d.end() - 12);
  if (d.empty()) {
    text("No drives yet", CX, CY, &fSmall, C_DIM);
    return;
  }
  float top = 0, mi = 0, gal = 0;
  for (auto &r : d) top = max(top, r.miles / r.gallons), mi += r.miles, gal += r.gallons;
  float avg = mi / gal, ymax = roundUp(top * 1.1f, 5);
  float slot = (PX1 - PX0) / (float)d.size();
  int w = max(3, min(14, (int)slot - 3));
  canvas.drawFastHLine(PX0 - 4, PY1, PX1 - PX0 + 8, rgb(C_TICK));
  for (size_t k = 0; k < d.size(); k++) {
    int x = PX0 + slot * k + (slot - w) / 2;
    column(x, w, d[k].miles / d[k].gallons / ymax, k == d.size() - 1 ? C_TEXT : C_BLUE);
  }
  int ay = PY1 - avg / ymax * (PY1 - PY0);
  canvas.drawFastHLine(PX0 - 4, ay, PX1 - PX0 + 8, rgb(0xB8BCC4));
  text("avg " + String(avg, 1), CX, PY0 - 12, &fSmall, C_TEXT);
  const DriveRec &last = d.back();
  text("latest " + String(last.miles / last.gallons, 1) + " MPG, " + String(last.miles, 1) + " mi", CX, 186, &fSmall, C_DIM);
}

static void drawTrendChart() {
  std::vector<DriveRec> all = hist.recentDrives(60);
  std::vector<DriveRec> d;
  for (auto &r : all)
    if (r.miles >= 0.5f && r.gallons > 0.05f) d.push_back(r);
  if (d.size() > 30) d.erase(d.begin(), d.end() - 30);
  if (d.size() < 2) {
    text("Needs a few drives", CX, CY, &fSmall, C_DIM);
    return;
  }
  // Fuel-weighted average of each drive and the four before it.
  std::vector<float> v;
  for (size_t i = 0; i < d.size(); i++) {
    float m = 0, g = 0;
    for (size_t j = i >= 4 ? i - 4 : 0; j <= i; j++) m += d[j].miles, g += d[j].gallons;
    v.push_back(m / g);
  }
  float lo = *std::min_element(v.begin(), v.end()), hi = *std::max_element(v.begin(), v.end());
  lo = floorf(lo - 1), hi = ceilf(hi + 1);
  auto X = [&](size_t i) { return PX0 + (PX1 - PX0) * (float)i / (v.size() - 1); };
  auto Y = [&](float m) { return PY1 - (m - lo) / (hi - lo) * (PY1 - PY0); };
  canvas.drawFastHLine(PX0 - 4, PY1, PX1 - PX0 + 8, rgb(C_TICK));
  canvas.drawFastHLine(PX0 - 4, PY0, PX1 - PX0 + 8, rgb(C_TRACK));
  text(String(hi, 0), PX0 - 16, PY0, &fSmall, C_DIM);
  text(String(lo, 0), PX0 - 16, PY1, &fSmall, C_DIM);
  for (size_t i = 1; i < v.size(); i++) canvas.drawWideLine(X(i - 1), Y(v[i - 1]), X(i), Y(v[i]), 2.2f, rgb(C_BLUE));
  canvas.fillSmoothCircle(X(v.size() - 1), Y(v.back()), 6, TFT_BLACK);
  canvas.fillSmoothCircle(X(v.size() - 1), Y(v.back()), 4, rgb(C_BLUE));
  float change = v.back() - v.front();
  text("now " + String(v.back(), 1) + " MPG", CX, PY0 - 12, &fSmall, C_TEXT);
  text((change >= 0 ? "+" : "") + String(change, 1) + " over " + String(d.size()) + " drives", CX, 186, &fSmall,
       change >= 0 ? C_GOOD : C_WARN);
}

static void drawHistoryCharts() {
  static const char *TITLES[HIST_CHARTS] = {"MPG BY SPEED", "LAST 12 DRIVES", "MPG TREND"};
  text(TITLES[histChart], CX, 40, &fSmall, C_DIM);
  switch (histChart) {
    case 0: drawSpeedChart(); break;
    case 1: drawRecentChart(); break;
    default: drawTrendChart(); break;
  }
  constexpr int GAP = 12;
  int x0 = CX - (HIST_CHARTS - 1) * GAP / 2;
  for (int i = 0; i < HIST_CHARTS; i++)
    canvas.fillSmoothCircle(x0 + i * GAP, 222, i == histChart ? 3 : 2, rgb(i == histChart ? C_TEXT : C_TICK));
}

static void drawCarBadge() {
  const char *alert = carAlert(car);
  if (alert) { text(alert, CX, 34, &fSmall, C_BAD); return; }
  int due = serviceDue();
  if (due < 0 || carPage == CP_SERVICE) return;
  const ServiceItem &it = maint.items[due];
  String b = it.name + (maint.remaining(it, car.odoMiles) < 0 ? " OVERDUE" : " DUE");
  b.toUpperCase();
  canvas.setFont(&fSmall);
  text(canvas.textWidth(b) <= chordWidth(34, 50) ? b : String("SERVICE DUE"), CX, 34, &fSmall, C_WARN);
}

static void drawCarScreen() {
  switch (carPage) {
    case CP_ECONOMY: drawEconomyPage(); break;
    case CP_TRIP: drawTripPage(); break;
    case CP_ENGINE: drawEnginePage(); break;
    case CP_CODES: drawCodesPage(); break;
    case CP_HEALTH: drawHealthPage(); break;
    case CP_SERVICE: drawServicePage(); break;
    default: drawHistoryPage(); break;
  }
  if (car.simulated && carPage != CP_SERVICE && carPage != CP_HEALTH) text("DEMO CAR", CX, 204, &fSmall, C_WARN);
  drawCarBadge();
  drawPageDots();
}

static void render(uint32_t now) {
  if (mode != MODE_HORNS) canvas.fillScreen(TFT_BLACK);
  switch (mode) {
    case MODE_HORNS: drawHornScreen(now); break;
    case MODE_CAR: drawCarScreen(); break;
    case MODE_MENU: drawMenuScreen(); break;
    case MODE_VOLUME: drawGauge("VOLUME", cfg.volumePct, C_VOLUME); break;
    case MODE_BRIGHTNESS: drawGauge("BRIGHTNESS", cfg.brightnessPct, C_BRIGHT); break;
    case MODE_WIFI: drawWifiScreen(); break;
    case MODE_FILLUP: drawFillupScreen(); break;
    case MODE_NUMBER: drawNumberScreen(); break;
    case MODE_HISTORY: drawHistoryCharts(); break;
    case MODE_DETAIL: drawDetailScreen(); break;
    case MODE_CONFIRM: drawConfirmScreen(); break;
    case MODE_OTA: drawOtaScreen(); break;
    case MODE_TEXT: drawTextScreen(); break;
  }
  drawToast();
  canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------- settings tree
static const char *COLOR_NAMES[] = {"Orange", "Cyan", "Lime", "Pink", "Yellow", "Violet", "Red", "Teal"};

static String colorName(const String &hex) {
  if (!hex.length()) return "Auto";
  uint32_t c = parseColor(hex, 0);
  for (int i = 0; i < N_ACCENTS; i++)
    if (ACCENTS[i] == c) return COLOR_NAMES[i];
  return "Custom";
}

static String hexOf(uint32_t c) {
  char b[8];
  snprintf(b, sizeof(b), "#%06lx", (unsigned long)c);
  return b;
}

static HornMeta *hornMeta(const String &key) { return cfg.find(key); }

static void saveHornChange() {
  cfg.saveHorns();
  rebuildHorns();
}

// Horns in the user's order that exist on the sound board (hidden ones too).
static std::vector<String> hornKeys() {
  std::vector<String> keys;
  for (auto &m : cfg.horns)
    if (std::find(files.begin(), files.end(), m.key) != files.end()) keys.push_back(m.key);
  return keys;
}

static void moveHorn(const String &key, int dir) {
  std::vector<String> keys = hornKeys();
  int at = std::find(keys.begin(), keys.end(), key) - keys.begin();
  int to = at + dir;
  if (to < 0 || to >= (int)keys.size()) return;
  HornMeta *a = hornMeta(keys[at]), *b = hornMeta(keys[to]);
  std::swap(*a, *b);
  saveHornChange();
}

static std::vector<MenuItem> hornDetail(const String &key) {
  std::vector<MenuItem> v;
  v.push_back(MenuItem::toggle(
      "Show on dial", [key] { HornMeta *m = hornMeta(key); return m && !m->hidden; },
      [key] {
        HornMeta *m = hornMeta(key);
        if (m) m->hidden = !m->hidden;
        saveHornChange();
      }));
  MenuItem color = MenuItem::choice(
      "Color", [key] { HornMeta *m = hornMeta(key); return m ? colorName(m->color) : String(); },
      [key] {
        HornMeta *m = hornMeta(key);
        if (!m) return;
        // Auto -> palette colours in order -> Auto
        int at = -1;
        uint32_t c = parseColor(m->color, 0);
        for (int i = 0; i < N_ACCENTS; i++)
          if (m->color.length() && ACCENTS[i] == c) at = i;
        m->color = at + 1 < N_ACCENTS ? hexOf(ACCENTS[at + 1]) : String();
        saveHornChange();
      });
  color.valueColor = [key] {
    for (auto &h : horns)
      if (h.key == key) return h.color;
    HornMeta *m = hornMeta(key);
    return m ? parseColor(m->color, C_DIM) : C_DIM;
  };
  v.push_back(color);
  v.push_back(MenuItem::act("Play", [key] {
    for (size_t i = 0; i < horns.size(); i++)
      if (horns[i].key == key) { playHorn(i); return; }
    int t = trackOf(key);
    if (t >= 0 && !demoMode) { playingHorn = -1; fx.play(t); }
  }));
  auto position = [key] {
    std::vector<String> keys = hornKeys();
    int at = std::find(keys.begin(), keys.end(), key) - keys.begin();
    return String(String(at + 1) + " of " + String(keys.size()));
  };
  v.push_back(MenuItem::act("Move up", [key] { moveHorn(key, -1); }, position));
  v.push_back(MenuItem::act("Move down", [key] { moveHorn(key, 1); }, position));
  v.push_back(MenuItem::info("Rename", [] { return String("on the phone page"); }));
  return v;
}

static std::vector<MenuItem> hornsMenu() {
  std::vector<MenuItem> v;
  if (demoMode) {
    v.push_back(MenuItem::info("No sound board", [] { return String("connect it to edit horns"); }));
    return v;
  }
  for (auto &key : hornKeys()) {
    HornMeta *m = hornMeta(key);
    String name = m && m->name.length() ? m->name : autoName(key);
    v.push_back(MenuItem::sub(name, [key] { return hornDetail(key); },
                              [key] { HornMeta *m = hornMeta(key); return String(m && m->hidden ? "Hidden" : "Shown"); }));
  }
  return v;
}

// What the last transmission check found, in full.
static String transDetail() {
  switch (car.trans) {
    case ObdData::TR_DONE: {
      if (!car.nTransDtcs) return "The transmission computer has no codes stored.";
      String s;
      for (int i = 0; i < car.nTransDtcs; i++) {
        String code = ObdData::dtcText(car.transDtcs[i]);
        const char *d = dtcDescription(code);
        s += code + (car.transNow[i] ? " (now): " : " (stored): ") + (d ? d : "look this code up online") + "\n\n";
      }
      return s;
    }
    case ObdData::TR_READING: return "Asking the transmission computer. Readings pause for a few seconds.";
    case ObdData::TR_NO_ANSWER:
      return "The transmission computer didn't answer. The event log on the phone page shows what was tried.";
    case ObdData::TR_REFUSED:
      return "The transmission computer answered but wouldn't list its codes. The event log on the phone page has its reply.";
    case ObdData::TR_MOVING: return "Stop the car first: readings pause while the transmission is asked.";
    case ObdData::TR_CAR_OFF: return "Turn the ignition on and wait for the adapter to connect.";
    default: return "Not checked yet. Choose Check transmission with the car stopped and the ignition on.";
  }
}

static std::vector<MenuItem> codesMenu() {
  std::vector<MenuItem> v;
  if (!car.dtcsRead) v.push_back(MenuItem::info("Not read yet", [] { return String("connect the OBD adapter"); }));
  for (int i = 0; i < car.nDtcs; i++) {
    String code = ObdData::dtcText(car.dtcs[i]);
    const char *d = dtcDescription(code);
    String desc = d ? d : "look this code up online";
    v.push_back(MenuItem::info(code, [desc] { return desc; }));
  }
  if (car.dtcsRead && car.nDtcs == 0) v.push_back(MenuItem::info("No trouble codes", [] { return String("all clear"); }));
  // The transmission computer keeps its own list, read only when asked.
  MenuItem tr = MenuItem::info("Transmission", [] { return car.transText(); }, transDetail);
  tr.valueColor = [] {
    return car.trans == ObdData::TR_DONE ? (car.nTransDtcs ? C_WARN : C_GOOD) : C_DIM;
  };
  v.push_back(tr);
  v.push_back(MenuItem::act("Check transmission", [] { showToast(startTransCheck()); },
                            [] { return String("car stopped, up to 30 s"); }));
  if (obd.isReadOnly())
    v.push_back(MenuItem::info("Clearing disabled", [] { return String("read-only mode is on"); }));
  else if (car.nDtcs > 0 || car.milOn)
    v.push_back(MenuItem::act("Clear codes", [] { obd.clearCodes(); showToast("Clearing codes"); }, nullptr,
                              "Clear all codes?"));
  return v;
}

// MPG calibration, in plain words (shared with the phone page's wording).
// This tank was tracked from a logged full fill-up with nothing missed, so the next fill-up can calibrate.
static bool trackedFromFull() { return car.tankFromFull && car.tankStartUsed == 0 && !car.tankIncomplete; }

static String calibrationStatus() {
  if (car.calFills > 0)
    return "calibrated, " + String(car.calFills) + (car.calFills == 1 ? " fill-up" : " fill-ups") + " (x" +
           String(car.fuelCal, 2) + ")";
  if (fabsf(car.fuelCal - 1) > 0.005f) return "adjusted (x" + String(car.fuelCal, 2) + ")";
  return trackedFromFull() ? String("calibrates at next fill-up") : String("after 2 full fill-ups");
}

// The full story behind "MPG accuracy", for its text screen.
static String mpgAccuracyText() {
  String s;
  if (car.calFills > 0)
    s = "Calibrated from " + String(car.calFills) + (car.calFills == 1 ? " fill-up" : " fill-ups") +
        ": fuel use is scaled x" + String(car.fuelCal, 2) + " to match the pump.";
  else if (fabsf(car.fuelCal - 1) > 0.005f)
    s = "Set by hand to x" + String(car.fuelCal, 2) + ". No fill-up has calibrated it yet.";
  else
    s = "Not calibrated yet. MPG is worked out from the engine's air flow, usually within about 10%.";
  s += "\n\nTank: ";
  if (!car.tankFromFull)
    s += "level unknown, so no range yet. Set Fuel level from your gauge, or log your next fill-up.";
  else if (car.tankStartUsed > 0)
    s += "started from the gauge. Fine for range, but too rough to calibrate with.";
  else if (car.tankIncomplete)
    s += "some driving wasn't recorded (5+ min without the adapter). If nothing was missed, use Mark as tracked.";
  else
    s += "tracked since a full fill-up.";
  s += "\n\nNext: fill up until the pump clicks off, then log the gallons.";
  if (trackedFromFull())
    s += car.calFills ? " Each one refines the calibration." : " The display compares its count with the pump and calibrates.";
  else
    s += " That starts a tracked tank; the full fill-up after it calibrates.";
  return s;
}

static String lastFillDetail() {
  String counts = "Display counted " + String(car.lastFillEstimate, 1) + " gal, pump said " +
                  String(car.lastFillPumped, 1) + " gal. ";
  switch (car.lastFill) {
    case ObdData::FILL_CALIBRATED: return counts + "Calibration moved halfway toward the pump.";
    case ObdData::FILL_STARTED: return "It started a tracked tank. The next full fill-up can calibrate.";
    case ObdData::FILL_UNTRACKED: return "Some driving before it wasn't recorded, so it wasn't used to calibrate.";
    case ObdData::FILL_TOO_DIFFERENT: return counts + "Too far apart to trust (partial fill?), so it wasn't used.";
    case ObdData::FILL_TOO_SMALL: return "Under 3 gallons is too little to compare, so it wasn't used.";
    default: return "";
  }
}

static String lastFillText() {
  switch (car.lastFill) {
    case ObdData::FILL_CALIBRATED:
      return "est " + String(car.lastFillEstimate, 1) + " vs pump " + String(car.lastFillPumped, 1) + " gal";
    case ObdData::FILL_STARTED: return "started tracking";
    case ObdData::FILL_UNTRACKED: return "skipped: untracked driving";
    case ObdData::FILL_TOO_DIFFERENT: return "skipped: partial fill?";
    case ObdData::FILL_TOO_SMALL: return "skipped: too few gallons";
    default: return "";
  }
}

static std::vector<MenuItem> fuelMenu() {
  std::vector<MenuItem> v;
  v.push_back(MenuItem::info("MPG accuracy", calibrationStatus, mpgAccuracyText));
  // Start the tank from the fuel gauge instead of waiting for a fill-up.
  MenuItem level = MenuItem::number(
      "Fuel level", [] { return car.tankFromFull ? roundf(car.tankFraction() * 20) * 5 : 50.0f; },
      [](float pct) { obd.setFuelLevel(pct); }, 0, 100, 5, 0, "% on the gauge");
  level.value = [] {
    if (!car.tankFromFull) return String("unknown: set from gauge");
    float r = car.rangeMiles();
    return String("~" + String(car.tankFraction() * 100, 0) + "%" + (isnan(r) ? String() : ", " + String(r, 0) + " mi left"));
  };
  v.push_back(level);
  if (car.lastFill != ObdData::FILL_NONE) v.push_back(MenuItem::info("Last fill-up", lastFillText, lastFillDetail));
  if (car.calFills > 0 || fabsf(car.fuelCal - 1) > 0.005f)
    v.push_back(MenuItem::act("Start over", [] { obd.resetCalibration(); showToast("Calibration reset"); },
                              [] { return String("forget what fill-ups taught it"); }, "Reset calibration?"));
  if (car.tankIncomplete)
    v.push_back(MenuItem::act("Mark as tracked", [] { obd.markTankComplete(); showToast("Marked as tracked"); }));
  v.push_back(MenuItem::number("Tank size", [] { return car.tankCapacity; }, [](float g) { obd.setTankCapacity(g); }, 5, 60,
                               0.1f, 1, "gal"));
  v.push_back(MenuItem::number("Engine size", [] { return obd.displacementL(); },
                               [](float l) { obd.setDisplacement(l); }, 1, 8, 0.1f, 1, "L"));
  return v;
}

static std::vector<MenuItem> adapterMenu() {
  std::vector<MenuItem> v;
  v.push_back(MenuItem::info("Status", [] { return String(car.stateText()); }));
  if (car.adapter[0]) v.push_back(MenuItem::info("Adapter", [] { return String(car.adapter); }));
  v.push_back(MenuItem::toggle("Read-only mode", [] { return obd.isReadOnly(); },
                               [] { obd.setReadOnly(!obd.isReadOnly()); }));
  v.push_back(MenuItem::toggle("Demo car", [] { return obd.simulating(); },
                               [] { obd.setSimulate(!obd.simulating()); }));
  v.push_back(MenuItem::act("Forget adapter", [] { obd.forgetAdapter(); showToast("Adapter forgotten"); }, nullptr,
                            "Forget adapter?"));
  return v;
}

static std::vector<MenuItem> healthMenu() {
  std::vector<MenuItem> v;
  std::vector<Health::Result> r = health.results();
  // Worst first; checks that don't apply to this car are left out.
  for (int level = Health::CHECK; level >= Health::NEEDS_DATA; level--) {
    for (int i = 0; i < (int)r.size(); i++) {
      if (r[i].status != level) continue;
      MenuItem it = MenuItem::info(
          Health::checkName(i), [i] { return health.result(i).summary; },
          [i] {
            Health::Result x = health.result(i);
            String s = Health::statusName(x.status);
            s.toUpperCase();
            return s + ": " + x.summary + "\n\n" + x.detail;
          });
      it.valueColor = [i] { return healthColor(health.result(i).status); };
      v.push_back(it);
    }
  }
  v.push_back(MenuItem::info("How it works", [] { return String(health.drives()) + " drives checked"; }, [] {
    return String("Watches fuel trims, idle, temperatures, timing, highway RPM, MPG and voltage for early signs of "
                  "worn or failing parts.\n\nFixed limits apply right away. Comparisons with this car's own "
                  "normal start after " + String(Health::LEARN_DRIVES) + " drives.\n\nThese point at likely "
                  "suspects, not a diagnosis. After a repair, use Relearn normal.");
  }));
  v.push_back(MenuItem::act("Relearn normal", [] { health.relearn(); showToast("Relearning"); },
                            [] { return String("after a repair"); }, "Relearn normal?"));
  return v;
}

static std::vector<MenuItem> carMenu() {
  std::vector<MenuItem> v;
  v.push_back(MenuItem::act("Log fill-up", [] { enterMode(MODE_FILLUP); }, [] {
    return String(car.tankUsed() >= 0.1f ? "~" + String(car.tankUsed(), 1) + " gal used" : String("resets the tank"));
  }));
  // Like the dash's trip odometer: miles and MPG since you last zeroed it.
  v.push_back(MenuItem::act("Reset trip meter", [] { obd.resetTrip(); tripResetAt = millis(); showToast("Trip reset"); },
                            [] {
                              float m = car.trip.mpg();
                              return String(car.trip.miles, 1) + " mi, " + (m > 0 ? String(m, 1) : String("--")) + " MPG";
                            },
                            "Reset trip meter?"));
  v.push_back(MenuItem::sub("Trouble codes", codesMenu, [] {
    if (!car.dtcsRead) return String("not read yet");
    int n = allCodes().size();
    return String(n ? String(n) + (n == 1 ? " code" : " codes") : String("none"));
  }));
  MenuItem hm = MenuItem::sub("Engine health", healthMenu, healthSummaryText);
  hm.valueColor = [] {
    Health::Overview o = healthOverview();
    return o.check ? C_BAD : o.watch ? C_WARN : o.ok ? C_GOOD : C_DIM;
  };
  v.push_back(hm);
  v.push_back(MenuItem::sub("Fuel & MPG", fuelMenu, [] { return String("calibration, tank, engine"); }));
  v.push_back(MenuItem::sub("OBD adapter", adapterMenu, [] {
    return String(car.simulated ? "demo car" : car.stateText());
  }));
  return v;
}

static std::vector<MenuItem> serviceLogMenu() {
  std::vector<MenuItem> v;
  for (size_t i = 0; i < maint.items.size(); i++) {
    MenuItem it = MenuItem::act(
        maint.items[i].name,
        [i] {
          maint.markDone(i, car.odoMiles);
          showToast("Logged " + maint.items[i].name);
        },
        [i] { return i < maint.items.size() ? serviceStatus(maint.items[i]) : String(); }, "Mark as done?");
    it.valueColor = [i] {
      float r = i < maint.items.size() ? maint.remaining(maint.items[i], car.odoMiles) : NAN;
      return isnan(r) ? C_DIM : serviceColor(r);
    };
    v.push_back(it);
  }
  return v;
}

static std::vector<MenuItem> intervalsMenu() {
  std::vector<MenuItem> v;
  for (size_t i = 0; i < maint.items.size(); i++)
    v.push_back(MenuItem::number(
        maint.items[i].name, [i] { return i < maint.items.size() ? (float)maint.items[i].interval : 0.0f; },
        [i](float mi) {
          if (i < maint.items.size()) maint.items[i].interval = mi;
          maint.save();
        },
        500, 100000, 500, 0, "miles"));
  return v;
}

static std::vector<MenuItem> serviceMenu() {
  std::vector<MenuItem> v;
  v.push_back(MenuItem::sub("Log a service", serviceLogMenu, [] {
    int u = maint.mostUrgent(car.odoMiles);
    return String(u < 0 ? String("mark items as done") : maint.items[u].name + ": " + serviceStatus(maint.items[u]));
  }));
  MenuItem odo = MenuItem::number(
      "Odometer",
      [] { return maint.odometerKnown() ? roundf(maint.odometer(car.odoMiles)) : 150000.0f; },
      [](float v) { pendingToast = setOdometerReading(v); }, 0, 999999, 1, 0, "miles", true);
  odo.value = [] { return String(maint.odometerKnown() ? withCommas(maint.odometer(car.odoMiles)) + " mi" : String("not set")); };
  v.push_back(odo);
  MenuItem sc = MenuItem::number("Speed correction", [] { return roundf(speedCorrectionPct() * 10) / 10; },
                                 [](float p) { setSpeedCorrectionPct(p); }, -15, 15, 0.1f, 1, "% faster than the car says");
  sc.value = speedCorrectionText;
  v.push_back(sc);
  v.push_back(MenuItem::sub("Intervals", intervalsMenu, [] { return String("miles between services"); }));
  return v;
}

static std::vector<MenuItem> rootMenu() {
  std::vector<MenuItem> v;
  v.push_back(MenuItem::sub("Sound", [] {
    return std::vector<MenuItem>{
        MenuItem::act("Volume", [] { enterMode(MODE_VOLUME); }, [] { return String(String(cfg.volumePct) + "%"); }),
        MenuItem::act("Test horn", [] { playHorn(selected()); },
                      [] { return horns.empty() ? String() : horns[selected()].label; }),
    };
  }, [] { return String("volume " + String(cfg.volumePct) + "%"); }));
  v.push_back(MenuItem::sub("Display", [] {
    static const char *TINTS[] = {"Black", "Subtle color", "Bold color"};
    return std::vector<MenuItem>{
        MenuItem::act("Brightness", [] { enterMode(MODE_BRIGHTNESS); }, [] { return String(String(cfg.brightnessPct) + "%"); }),
        MenuItem::choice("Dim after", [] { return dimAfterText(cfg.dimAfterSec); },
                         [] {
                           constexpr int N = sizeof(DIM_CHOICES) / sizeof(DIM_CHOICES[0]);
                           int i = 0;
                           while (i < N && DIM_CHOICES[i] != cfg.dimAfterSec) i++;
                           cfg.dimAfterSec = DIM_CHOICES[(i + 1) % N];  // unknown value: start over at the first
                           cfg.saveDim();
                         }),
        MenuItem::number("Dim brightness", [] { return (float)cfg.dimPct; },
                         [](float p) { cfg.dimPct = p; cfg.saveDim(); }, 10, 100, 5, 0, "%"),
        MenuItem::choice("Horn background", [] { return String(TINTS[cfg.tint]); },
                         [] { cfg.tint = (cfg.tint + 1) % 3; cfg.saveTint(); }),
        MenuItem::toggle("LED ring", [] { return cfg.leds; }, [] { cfg.leds = !cfg.leds; cfg.saveLeds(); }),
    };
  }, [] { return String("brightness, dimming, LEDs"); }));
  v.push_back(MenuItem::sub("Horns", hornsMenu, [] {
    if (demoMode) return String("no sound board");
    size_t all = hornKeys().size();
    return String(String(horns.size()) + " of " + String(all) + " shown");
  }));
  v.push_back(MenuItem::sub("Car", carMenu, [] { return String("fill-ups, codes, adapter"); }));
  v.push_back(MenuItem::sub("Service", serviceMenu, [] {
    int u = serviceDue();
    return String(u < 0 ? String("reminders & odometer") : maint.items[u].name + " due");
  }));
  v.push_back(MenuItem::sub("Phone & Wi-Fi", [] {
    return std::vector<MenuItem>{
        MenuItem::act("Phone setup", [] { enterMode(MODE_WIFI); }, [] { return String("scan the QR code"); }),
        MenuItem::toggle("Wi-Fi", [] { return web.active(); }, [] {
          if (web.active()) web.stop();
          else web.start();
        }),
    };
  }, [] { return String(web.active() ? "Wi-Fi on" : "names, charts, updates"); }));
  v.push_back(MenuItem::sub("System", [] {
    return std::vector<MenuItem>{
        MenuItem::info("Firmware", [] { return String(FW_VERSION); }),
        MenuItem::info("Drive history", [] { return String(String(hist.driveCount()) + " drives recorded"); }),
        MenuItem::info("Storage", [] { return Storage::summary(); },
                       [] {
                         return String("Drive history, engine health and the event log are kept in the display's "
                                       "flash memory, and every save is read back to check it.\n\nIf this says "
                                       "PROBLEM, saves are failing: download a backup from the phone page, then use "
                                       "Repair storage below.");
                       }),
        MenuItem::info("Event log", [] { return String("on the phone page"); },
                       [] {
                         return String("Restarts, adapter connections, drives, fill-ups, warnings and setting changes are "
                                       "logged to help track down problems.\n\nRead it on the phone page: Phone & Wi-Fi > "
                                       "Phone setup, then Event log at the bottom.");
                       }),
        MenuItem::info("Backup", [] { return String("on the phone page"); },
                       [] {
                         return String("Save every setting, calibration, service record and the drive history to your "
                                       "phone, and restore it later.\n\nOpen Phone & Wi-Fi > Phone setup, then Backup "
                                       "near the bottom.");
                       }),
        MenuItem::act("Clear drive history", [] { hist.clear(); showToast("History cleared"); }, nullptr,
                      "Delete drive history?"),
        MenuItem::act("Repair storage", [] { showToast(repairStorage() ? "Storage repaired" : "Repair failed"); }, nullptr,
                      "Erase and rebuild storage?"),
        MenuItem::act("Restart", [] { ESP.restart(); }, nullptr, "Restart the display?"),
    };
  }, [] { return String(String("version ") + FW_VERSION); }));
  return v;
}

static void menuPress() {
  MenuItem it = menu.current();  // copy: actions may rebuild the list
  switch (it.kind) {
    case MenuItem::SUB:
      menu.push(it.label, it.children());
      break;
    case MenuItem::BACK:
      if (menu.pop()) menu.refresh();
      else enterMode(MODE_CAR);
      break;
    case MenuItem::NUMBER:
      editItem = it;
      editValue = it.get();
      mode = MODE_NUMBER;
      break;
    case MenuItem::ACTION:
      if (it.confirm.length()) {
        confirmQuestion = it.confirm;
        confirmSubject = it.label;
        confirmAction = it.action;
        mode = MODE_CONFIRM;
        break;
      }
      it.action();
      if (mode == MODE_MENU) menu.refresh();
      break;
    case MenuItem::TOGGLE:
    case MenuItem::CHOICE:
      it.action();
      if (mode == MODE_MENU) menu.refresh();
      break;
    case MenuItem::INFO: {
      String body = it.detail ? it.detail() : it.value ? it.value() : String();
      if (body.length()) openText(it.label, body);
      break;
    }
  }
}

// Press on a car page: open what that page has more of.
static void openDetails() {
  switch (carPage) {
    case CP_ECONOMY:
    case CP_ENGINE: enterMode(MODE_DETAIL); break;
    case CP_TRIP: enterMode(MODE_FILLUP); break;
    case CP_CODES:
      menu.openPath({"Car", "Trouble codes"});
      mode = MODE_MENU;
      break;
    case CP_HEALTH:
      menu.openPath({"Car", "Engine health"});
      mode = MODE_MENU;
      break;
    case CP_SERVICE:
      menu.openPath({"Service", "Log a service"});
      mode = MODE_MENU;
      break;
    default: enterMode(MODE_HISTORY); break;
  }
}

// Screens where a double-click means "horns". Elsewhere presses act at once.
static bool doubleClickHere() { return mode == MODE_CAR || mode == MODE_DETAIL || mode == MODE_HISTORY; }

// ---------------------------------------------------------------- input
static void handleInput(int32_t det, Press btn, uint32_t now) {
  switch (mode) {
    case MODE_HORNS:
      if (det && !horns.empty()) {
        hornWheel.target += det;
        selChangedAt = now;
        cfg.saveSelected(horns[selected()].key);
      }
      if (btn == PRESS_SHORT) playHorn(selected());
      if (btn == PRESS_LONG) enterMode(MODE_MENU);
      break;

    case MODE_CAR:
      if (det) carPage = ((carPage + det) % CP_COUNT + CP_COUNT) % CP_COUNT;
      if (btn == PRESS_SHORT) openDetails();
      if (btn == PRESS_LONG) enterMode(MODE_MENU);
      if (btn == PRESS_DOUBLE) enterMode(MODE_HORNS);
      break;

    case MODE_DETAIL:
      if (btn == PRESS_SHORT) enterMode(MODE_CAR);
      if (btn == PRESS_LONG) enterMode(MODE_MENU);
      if (btn == PRESS_DOUBLE) enterMode(MODE_HORNS);
      break;

    case MODE_FILLUP:
      if (det) {
        // Fast turning moves in whole gallons, slow turning in tenths.
        float step = now - lastDetentAt < 60 ? 1.0f : 0.1f;
        fillGallons = constrain(roundf((fillGallons + det * step) * 10) / 10, 0.0f, car.tankCapacity + 5);
      }
      if (btn == PRESS_SHORT && fillGallons >= 0.5f) {
        obd.filledUp(fillGallons);
        showToast("Fill-up saved");
        backOrHome();
      } else if (btn) {
        showToast("Cancelled");
        backOrHome();
      }
      break;

    case MODE_HISTORY:
      if (det) histChart = ((histChart + det) % HIST_CHARTS + HIST_CHARTS) % HIST_CHARTS;
      if (btn == PRESS_SHORT) enterMode(MODE_CAR);
      if (btn == PRESS_LONG) enterMode(MODE_MENU);
      if (btn == PRESS_DOUBLE) enterMode(MODE_HORNS);
      break;

    case MODE_NUMBER:
      if (det) {
        // Spinning faster takes bigger steps (x10, and for big numbers x100/x1000).
        uint32_t gap = now - lastDetentAt;
        float mult = 1;
        if (editItem.accelerate) mult = gap < 25 ? 1000 : gap < 45 ? 100 : gap < 90 ? 10 : 1;
        else if (gap < 60) mult = 10;
        float st = editItem.step * mult;
        editValue = constrain(roundf((editValue + det * st) / editItem.step) * editItem.step, editItem.min, editItem.max);
      }
      if (btn == PRESS_SHORT) {
        pendingToast = "";
        editItem.set(editValue);
        showToast(pendingToast.length() ? pendingToast : String("Saved"));
        backOrHome();
      } else if (btn == PRESS_LONG) {
        showToast("Cancelled");
        backOrHome();
      }
      break;

    case MODE_CONFIRM:
      if (btn == PRESS_SHORT) {
        mode = MODE_MENU;  // actions may switch screens themselves
        if (confirmAction) confirmAction();
        if (mode == MODE_MENU) backOrHome();
      } else if (btn == PRESS_LONG) {
        backOrHome();
      }
      break;

    case MODE_OTA:
      break;

    case MODE_TEXT:
      if (det) textScroll = constrain(textScroll + det, 0, textMaxScroll());
      if (btn) backOrHome();
      break;

    case MODE_MENU:
      if (!menu.open()) { enterMode(MODE_CAR); break; }
      if (det) menu.top().wheel.target += det;
      if (btn == PRESS_LONG) {
        if (menu.pop()) menu.refresh();  // back one level
        else enterMode(MODE_CAR);
      }
      if (btn == PRESS_SHORT) menuPress();
      break;

    case MODE_VOLUME:
      if (det) {
        cfg.volumePct = constrain(cfg.volumePct + det * PCT_STEP, 0, 100);
        applyVolume();
      }
      if (btn) backOrHome();
      break;

    case MODE_BRIGHTNESS:
      if (det) {
        cfg.brightnessPct = constrain(cfg.brightnessPct + det * PCT_STEP, 10, 100);
        setBacklight(backlightLevel());
      }
      if (btn) backOrHome();
      break;

    case MODE_WIFI:
      if (btn) backOrHome();
      break;
  }
}

// ---------------------------------------------------------------- console
// USB console commands (for development):
//   ">cmd"  raw command to the Audio FX     list    rescan sound files
//   cw [n] / ccw [n]  turn the knob           press / long   push the knob
//   swipe l|r  touch swipe                    obdsim 0|1  demo car on/off
//   shot    dump the frame buffer (RGB565)   log 0|1  Audio FX link logging
//   status  connection summary               files   what's in the flash file store
//   events  the event log                    hist    drive history (JSON)
//   fsput <path> <offset> <hex> / fsrm <path>   write / delete a file      restart
#ifdef SORENTO_SIM
// Simulator testing for the health checks: feed them synthetic 15-minute
// drives (city with a cold start, idle, highway, a hard acceleration, idle).
// The first LEARN_DRIVES are normal; after that the named fault is added.
//   healthtest ok|leak|maf|fuel|rich|bank|idle|stall|thermo|hot|knock|trans|mpg|battery|tps|sensor [drives] [cut]   (cut: the last drive loses power instead of ending)
static void healthTest(const String &kind, int drives, bool cut = false) {
  health.setDemo(false);
  for (int n = 0; n < drives; n++) {
    bool fault = n >= Health::LEARN_DRIVES;
    auto is = [&](const char *k) { return fault && kind == k; };
    health.engineOff(is("battery") ? 11.9f : 12.5f);
    health.beginDrive(true);
    float coolant = 60, lastSlow = -10, lastDetail = -10;
    for (float t = 0; t < 900; t += 0.5f) {
      bool idle = false, hard = t >= 780 && t < 800;
      float mph;
      if (t < 420) idle = fmodf(t, 90) < 30, mph = idle ? 0 : 30;
      else if (t < 480 || (t >= 800 && t < 860)) idle = true, mph = 0;
      else if (t < 780) mph = 62;
      else if (hard) mph = 40 + (t - 780);
      else mph = 30;
      float load = idle ? 22 : hard ? 85 : mph > 50 ? 45 : 35;
      float rpm = idle ? 700 + 6 * sinf(t * 1.7f) : mph * (mph > 50 ? 31 : 45);
      if (!idle && mph > 50 && !hard && is("trans")) rpm *= 1.09f;
      if (idle && is("idle")) rpm = 720 + 160 * sinf(t * 0.9f);
      if (is("stall") && t >= 830 && t < 833) rpm = 0;
      if (hard) rpm = 3200;
      float maf = idle ? (is("maf") ? 3.0f : 4.5f) : mph * 0.4f + (hard ? 60 : 0);
      float gph = idle ? 0.5f : hard ? 8 : mph > 50 ? 2.8f * (is("mpg") ? 1.2f : 1) : 1.5f;
      // Coolant heads for its target at up to 0.5 F/s, like a real engine.
      float target = is("thermo") && t > 540 && t < 780 ? 155 : is("hot") && t > 500 && t < 780 ? 232 : 195;
      coolant += constrain(target - coolant, -0.25f, 0.25f);
      float cool = coolant;
      health.fast(0.5f, mph, rpm, maf, gph);
      if (t - lastSlow >= 2) {
        lastSlow = t;
        health.slow(is("sensor") && t >= 600 && t < 602 ? -40 : cool, 70, load, 14.0f);
      }
      if (t - lastDetail >= (hard ? 2.5f : 10)) {
        lastDetail = t;
        float add = idle ? (is("leak") ? 13 : is("maf") ? 2 : 0) : hard ? (is("maf") ? 9 : is("fuel") ? 13 : 0)
                                                                         : (is("maf") ? 10 : is("fuel") ? 2 : 0);
        if (is("rich")) add = -17;
        float st[2] = {1.5f * sinf(t), 1.5f * cosf(t)};
        float lt[2] = {3 + add + (is("bank") ? 10 : 0), 3 + add + (is("bank") ? -3 : 0)};
        float timing = idle ? 12 : hard ? 20 : is("knock") ? 24 : 30;
        float throttle = idle ? (is("tps") ? 14 : 8) : hard ? 80 : 18;
        health.details(st, lt, timing, throttle, mph, rpm);
      }
    }
    if (!(cut && n == drives - 1)) health.endDrive();  // "cut": the last drive loses power instead of ending
  }
  Serial.printf("healthtest %s: %d drives\n", kind.c_str(), drives);
}
#endif

// Known files in the store, with sizes (console "files").
static void printFiles() {
  Serial.printf("storage %s: mounted=%d formatted=%d writes=%lu failures=%lu used=%u of %u bytes\n",
                Storage::summary().c_str(), Storage::mounted, Storage::formatted, (unsigned long)Storage::writes,
                (unsigned long)Storage::failures, (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  for (const char *n : {"/events.log", "/events.old", "/drives.bin", "/fills.bin", "/speed.bin", "/health.bin", "/health_drive.bin",
                        "/demo_drives.bin", "/demo_fills.bin", "/demo_speed.bin", "/restore.json"})
    if (LittleFS.exists(n)) Serial.printf("  %-18s %u\n", n, (unsigned)Storage::size(n));
}

// "fsput <path> <offset> <hex>": write bytes into a file (offset 0 starts it
// again). For putting recovered history back over USB.
static void consolePut(const String &args) {
  int a = args.indexOf(' '), b = args.indexOf(' ', a + 1);
  if (a < 1 || b < 0) { Serial.println("ERR usage: fsput <path> <offset> <hex>"); return; }
  String path = args.substring(0, a), hex = args.substring(b + 1);
  size_t offset = args.substring(a + 1, b).toInt();
  std::vector<uint8_t> bytes;
  auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
  for (unsigned int i = 0; i + 1 < hex.length(); i += 2) {
    int hi = nib(hex[i]), lo = nib(hex[i + 1]);
    if (hi < 0 || lo < 0) { Serial.println("ERR bad hex"); return; }
    bytes.push_back(hi << 4 | lo);
  }
  bool ok;
  if (offset == 0) ok = Storage::writeFile(path, bytes.data(), bytes.size());
  else ok = Storage::size(path) == offset && Storage::appendFile(path, bytes.data(), bytes.size());
  Serial.printf("%s %s %u\n", ok ? "OK" : "ERR", path.c_str(), (unsigned)Storage::size(path));
}

static bool handleConsole(int32_t &det, Press &btn, int &swipe) {
  if (!Serial.available()) return false;
  String s = Serial.readStringUntil('\n');
  s.trim();
  int sp = s.indexOf(' ');
  int n = sp > 0 ? max(1, (int)s.substring(sp + 1).toInt()) : 1;
  if (s.startsWith(">")) fx.raw(s.substring(1));
  else if (s == "list") fx.requestList();
  else if (s.startsWith("cw")) det += n;
  else if (s.startsWith("ccw")) det -= n;
  else if (s == "press") btn = PRESS_SHORT;
  else if (s == "long") btn = PRESS_LONG;
  else if (s == "double") btn = PRESS_DOUBLE;
  else if (s == "swipe l") swipe = 1;
  else if (s == "swipe r") swipe = -1;
  else if (s == "obdsim 1") obd.setSimulate(true);
  else if (s == "obdsim 0") obd.setSimulate(false);
  else if (s == "tripclear") { obd.resetTrip(); obd.filledUp(0.01f); }  // zero trip + tank
  else if (s == "shot") return true;
  else if (s == "ver") Serial.println(FW_VERSION " " __DATE__ " " __TIME__);
  else if (s == "events" || s == "health") {
    Serial.setTxTimeoutMs(1000);  // long replies: wait for the PC instead of dropping the rest
    Serial.println(s == "events" ? webLogText() : webHealthJson());
    Serial.setTxTimeoutMs(0);
  }
#ifdef SORENTO_SIM
  else if (s == "healthreset") health.relearn();
  else if (s == "elmtrace 1" || s == "elmtrace 0") SimElm::trace = s.endsWith("1");  // print the demo adapter's traffic
  else if (s.startsWith("addmiles ")) obd.addTestMiles(s.substring(9).toFloat());
  else if (s.startsWith("healthtest ")) {
    String a = s.substring(11);
    int sp2 = a.indexOf(' ');
    bool cut = a.endsWith(" cut");
    if (cut) a = a.substring(0, a.length() - 4);
    sp2 = a.indexOf(' ');
    healthTest(sp2 > 0 ? a.substring(0, sp2) : a, sp2 > 0 ? a.substring(sp2 + 1).toInt() : 12, cut);
  }
#endif
  else if (s.startsWith("maint ")) webMaint(s.substring(6));  // same JSON as the phone page
  else if (s.startsWith("obd ")) webObd(s.substring(4));
  else if (s == "maintreset") maint.reset();
  else if (s.startsWith("setcal ")) obd.setCalibration(s.substring(7).toFloat());
  else if (s == "tankfull") obd.markTankFull();
  else if (s == "gapclear") obd.markTankComplete();
  else if (s == "histreload") hist.reload();
  else if (s == "files") printFiles();
  else if (s.startsWith("fsput ")) consolePut(s.substring(6));
  else if (s.startsWith("fsrm ")) Serial.println(Storage::remove(s.substring(5)) ? "OK" : "ERR");
  else if (s == "restart") ESP.restart();
  else if (s == "home") enterMode(MODE_CAR);
  else if (s.startsWith("page ")) { carPage = constrain((int)s.substring(5).toInt(), 0, CP_COUNT - 1); enterMode(MODE_CAR); }
  else if (s == "histdemo") Serial.println(hist.fillDemo() ? "demo history filled" : "demo car is off - refused");
  else if (s == "hist") {
    Serial.setTxTimeoutMs(1000);
    Serial.println(hist.json());
    Serial.setTxTimeoutMs(0);
  }
  else if (s.startsWith("simdrive ")) obd.simDrive(s.substring(9).toFloat());
  else if (s == "rf on") rf.enabled = true;
  else if (s == "rf off") rf.enabled = false;
  else if (s == "rf stat")
    Serial.printf("rf %s, %lu edges/s on GPIO%d\n", rf.enabled ? "on" : "off", (unsigned long)rf.edgesPerSecond(),
                  PIN_RF_DOUT);
  else if (s == "json") {
    Serial.setTxTimeoutMs(1000);
    Serial.println(webStateJson());
    Serial.setTxTimeoutMs(0);
  }
  else if (s == "log 0") fx.setLog(nullptr);
  else if (s == "log 1") fx.setLog(&Serial);
  else if (s == "status") {
    ObdData d = obd.snapshot();
    Serial.printf("fx %s rx=GPIO%d files=%d vol=%d reboots=%d | horns=%d demo=%d wifi=%d/%d\n",
                  fx.connected() ? "online" : "OFFLINE", fx.rxPin(), fx.fileCount(), fx.volume(), fx.reboots(),
                  (int)horns.size(), demoMode, web.active(), web.stations());
    Serial.printf("obd %s '%s' sim=%d maf=%d mph=%.1f rpm=%.0f gph=%.2f mpg=%.1f cool=%.0f V=%.1f mil=%d codes=%d "
                  "trip=%.2fmi/%.3fgal tank=%.1fmi/%.2fgal cal=%.2f fills=%d full=%d incomplete=%d odo=%.1f\n",
                  d.stateText(), d.adapter, d.simulated, d.useMaf, d.mph, d.rpm, d.gph, d.instMpg, d.coolantF, d.volts,
                  d.milOn, d.nDtcs, d.trip.miles, d.trip.gallons, d.tank.miles, d.tank.gallons, d.fuelCal, d.calFills, d.tankFromFull, d.tankIncomplete, d.odoMiles);
  }
  return false;
}

static void sendScreenshot() {
  Serial.setTxTimeoutMs(2000);
  Serial.print("\nSHOT240\n");
  Serial.write((const uint8_t *)canvas.getBuffer(), 240 * 240 * 2);
  Serial.flush();
  Serial.setTxTimeoutMs(0);
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never stall the UI when no PC is reading the console

  // Flash file store first: the event log, and a backup waiting to be restored
  // before any settings are read.
  Storage::begin();
  events.begin();
  events.add("Started v" FW_VERSION ": %s", EventLog::resetReason());
  if (Storage::formatted) events.add("Storage was blank or unreadable: started a new one");
  if (!Storage::mounted) events.add("Storage unavailable: nothing will be saved");
  Backup::applyStaged();

  pinMode(PIN_POWER_LED, OUTPUT);
  digitalWrite(PIN_POWER_LED, LOW);
  pinMode(PIN_LCD_PWR1, OUTPUT);
  digitalWrite(PIN_LCD_PWR1, HIGH);
  pinMode(PIN_LCD_PWR2, OUTPUT);
  digitalWrite(PIN_LCD_PWR2, HIGH);

  cfg.begin();

  lcd.init();
  lcd.fillScreen(TFT_BLACK);
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  canvas.createSprite(240, 240);
  loadFonts();

  ledcAttach(PIN_BACKLIGHT, 5000, 8);
  setBacklight(backlightLevel());

  ring.begin();
  ring.setBrightness(90);
  ring.clear();
  ring.show();

  pinMode(PIN_ENC_A, INPUT);
  pinMode(PIN_ENC_B, INPUT);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  encLast = ((uint8_t)digitalRead(PIN_ENC_A) << 1) | (uint8_t)digitalRead(PIN_ENC_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encoderISR, CHANGE);
  touch.begin(PIN_TP_SDA, PIN_TP_SCL, PIN_TP_RST);

  fx.begin(Serial1, PIN_FX_RX, PIN_FX_TX);
  rf.begin(PIN_RF_DOUT);
  applyVolume();

  web.begin();
  obd.setHistory(&hist);
  health.begin();
  obd.setHealth(&health);
  obd.begin();
  maint.begin();
  menu.root = rootMenu;
  rebuildHorns();  // from the cached file list until the board answers
  lastInput = millis();
  Serial.println("Sorento Dash ready");
}

void loop() {
  uint32_t now = millis();
  static uint32_t lastFrame = now;

  fx.poll();
  web.loop();
  events.poll();
  health.poll();
  // Saves that fail are said out loud, once (and logged, if the log still works).
  static bool storageWarned = false;
  if (!storageWarned && now > 10000 && !Storage::healthy()) {
    storageWarned = true;
    showToast("Storage problem");
    events.add("Storage problem: saves are failing (%lu so far)", (unsigned long)Storage::failures);
  }
  rf.poll(Serial);
  obd.setOnComputer(Serial.isPlugged());

  if (fx.listVersion() != shownListVersion) {
    shownListVersion = fx.listVersion();
    rebuildHorns();
    events.add("Sound board connected: %d files, %d horns shown", fx.fileCount(), (int)horns.size());
  }
  static bool fxWasOn = false, fxMissingLogged = false;
  if (fxWasOn && !fx.connected()) events.add("Sound board stopped answering");
  if (!fxWasOn && !fx.connected() && !fxMissingLogged && now > 15000) {
    if (shownListVersion == 0) events.add("Sound board not found");
    fxMissingLogged = true;
  }
  fxWasOn = fx.connected();

  if (demoPlaying && now - demoStart > DEMO_LEN_MS) demoPlaying = false;

  int32_t det = takeDetents();
  Press btn = button.poll(now);
  // Holding inside settings keeps backing out (to exit quickly); a hold that
  // opened settings from another screen doesn't continue into it.
  static bool holdBeganInMenu = false;
  if (btn == PRESS_LONG) holdBeganInMenu = mode == MODE_MENU || mode == MODE_TEXT;
  if (btn == PRESS_REPEAT) btn = holdBeganInMenu && mode == MODE_MENU ? PRESS_LONG : PRESS_NONE;
  // Double-click: hold a short press back briefly to see if a second follows.
  static bool pendingShort = false;
  static uint32_t pendingAt = 0;
  if (btn == PRESS_SHORT && doubleClickHere()) {
    if (pendingShort && now - pendingAt < DOUBLE_CLICK_MS) {
      btn = PRESS_DOUBLE;
      pendingShort = false;
    } else {
      pendingShort = true;
      pendingAt = now;
      btn = PRESS_NONE;
    }
  } else if (btn != PRESS_NONE) {
    pendingShort = false;  // a hold cancels a waiting click
  }
  if (pendingShort && now - pendingAt >= DOUBLE_CLICK_MS) {
    pendingShort = false;
    if (doubleClickHere()) btn = PRESS_SHORT;
  }
  int swipe = 0;
  switch (touch.poll(now)) {
    case Touch::SWIPE_LEFT: swipe = 1; break;
    case Touch::SWIPE_RIGHT: swipe = -1; break;
    default: break;
  }
  bool shot = handleConsole(det, btn, swipe);
  if (det || btn || swipe) {
    lastInput = now;
    if (dimmed) { dimmed = false; setBacklight(backlightLevel()); }
    if (det || btn) handleInput(det, btn, now);
    if (det) lastDetentAt = now;
    if (swipe) stepPage(swipe);
  }

  // Settings stay open until left; only the horn screen goes back home by itself.
  obd.setDetailFocus(mode == MODE_DETAIL && carPage == CP_ENGINE);
  if (mode == MODE_HORNS && !isPlaying() && now - lastInput > HORNS_TIMEOUT_MS) enterMode(MODE_CAR);

  // Phone setup Wi-Fi turns off once nobody is using it.
  if (web.active() && mode != MODE_WIFI && web.stations() == 0 && now - web.lastActivity() > WIFI_IDLE_OFF_MS &&
      now - lastInput > WIFI_IDLE_OFF_MS)
    web.stop();

  // Warnings go in the event log when they start, and once they've been gone
  // for 30 s (a reading hovering at a limit doesn't fill the log).
  static const char *loggedAlert = nullptr;
  static uint32_t alertClearSince = 0;
  if (const char *alert = carAlert(car)) {
    alertClearSince = 0;
    if (alert != loggedAlert)
      events.add("Warning %s: coolant %.0f F, %.1f V, %.0f rpm%s", alert, car.coolantF, car.volts, car.rpm,
                 car.simulated ? " (demo)" : "");
    loggedAlert = alert;
  } else if (loggedAlert) {
    if (!alertClearSince) alertClearSince = now;
    if (now - alertClearSince > 30000) {
      events.add("Warning over: %s", loggedAlert);
      loggedAlert = nullptr;
    }
  }

  // Dim when left alone (not while scanning the QR code, updating, or setting brightness).
  if (!dimmed && cfg.dimAfterSec > 0 && now - lastInput > cfg.dimAfterSec * 1000UL && !isPlaying() &&
      mode != MODE_WIFI && mode != MODE_OTA && mode != MODE_BRIGHTNESS && !carAlert(car)) {
    dimmed = true;
    setBacklight(dimLevel());
  }

  updateLeds(now);

  if (now - lastFrame >= 16) {
    float dt = (now - lastFrame) / 1000.0f;
    lastFrame = now;
    car = obd.snapshot();
    static ObdData::TransState lastTrans = ObdData::TR_NOT_READ;
    if (car.trans != lastTrans) {  // a transmission check finished (or couldn't start)
      lastTrans = car.trans;
      if (car.trans != ObdData::TR_READING && car.trans != ObdData::TR_NOT_READ) showToast("Transmission: " + car.transText());
    }
    hornWheel.animate(dt);
    if (menu.open()) menu.top().wheel.animate(dt);
    render(now);
  }
  if (shot) sendScreenshot();
}
