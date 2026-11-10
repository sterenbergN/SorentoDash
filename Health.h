// Engine health checks: early warnings for worn or failing parts, worked out
// from the readings the display already takes (fuel trims, RPM, airflow,
// temperatures, timing, throttle, voltage, speed).
//
// Two kinds of rules:
//  - fixed limits that mean trouble on any engine (fuel trims beyond +-10%,
//    an idle that hunts, an engine that cools down on the highway...), and
//  - drift from this car's own normal, learned over its first LEARN_DRIVES
//    drives (timing, gear ratio, MPG, battery...), so sensor quirks such as
//    an adapter that reads the voltage low don't matter.
// Each drive's averages feed the checks when it ends; one-off events (a
// stall, a sensor glitch) are scored and fade over a few clean drives.
//
// Fed from the OBD task, read by the UI and web page; a mutex keeps them apart.
// These point at likely suspects - they are not a diagnosis.
#pragma once
#include <Arduino.h>
#include <FFat.h>
#include <vector>
#include "EventLog.h"

// A per-drive average followed across drives.
struct HealthMetric {
  float base = NAN;    // this car's normal: the mean of its first LEARN_DRIVES drives
  float recent = NAN;  // the last few drives (moving average)
  uint16_t n = 0, pad = 0;
};

class Health {
public:
  static constexpr int LEARN_DRIVES = 8;
  static constexpr float WARM_F = 170;  // fuel trims mean little before the engine is warm (open loop)

  enum Metric : uint8_t {
    M_TRIM_IDLE1, M_TRIM_IDLE2, M_TRIM_CRUISE1, M_TRIM_CRUISE2, M_TRIM_LOAD1, M_TRIM_LOAD2,
    M_IDLE_RPM, M_IDLE_SD, M_IDLE_MAF, M_IDLE_TPS, M_CRUISE_TIMING, M_CRUISE_RATIO, M_RATIO_JITTER,
    M_HWY_MPG, M_WARMUP, M_REST_V, M_RUN_V, M_COUNT
  };
  enum Event : uint8_t { E_STALL, E_DIP, E_TPS_GLITCH, E_COOLANT_GLITCH, E_IAT_GLITCH, E_COLD_HWY, E_HOT, E_NO_WARM, E_COUNT };

  enum Check : uint8_t {
    C_AIR_LEAK, C_MAF, C_FUEL, C_RICH, C_BANKS, C_IDLE, C_THROTTLE, C_THERMOSTAT, C_TEMP_SENSORS, C_KNOCK,
    C_TRANS, C_MPG, C_BATTERY, C_COUNT
  };
  enum Status : uint8_t { NOT_APPLICABLE, NEEDS_DATA, LEARNING, OK, WATCH, CHECK };

  struct Result {
    Status status = NEEDS_DATA;
    String summary;  // a few words for the list
    String detail;   // what it means and what to look at
  };

  static const char *checkName(int c) {
    static const char *N[C_COUNT] = {"Air leaks", "Airflow sensor", "Fuel delivery", "Running rich", "Engine sides",
                                     "Idle", "Throttle sensor", "Thermostat", "Temp sensors", "Knock",
                                     "Transmission", "Highway MPG", "Battery"};
    return c >= 0 && c < C_COUNT ? N[c] : "";
  }
  static const char *statusName(Status s) {
    static const char *N[] = {"n/a", "needs data", "learning", "OK", "watch", "check"};
    return N[s];
  }

  // ---- setup (UI thread)
  void begin() {
    mtx = xSemaphoreCreateMutex();
    lock();
    load();
    evaluateLocked();
    unlock();
  }
  void setDisplacement(float l) { displacement = l; }
  // Demo car drives are ignored (they'd teach it nonsense).
  void setDemo(bool on) { demo = on; }

  // The speed correction changed by factor k: keep the speed-based normals
  // (RPM per mph, highway MPG) in step so it doesn't look like a change in the car.
  void rescaleSpeed(float k) {
    lock();
    for (float *v : {&st.m[M_CRUISE_RATIO].base, &st.m[M_CRUISE_RATIO].recent}) *v /= k;
    for (float *v : {&st.m[M_HWY_MPG].base, &st.m[M_HWY_MPG].recent}) *v *= k;
    evaluateLocked();
    saveLocked();
    unlock();
  }

  // Forget what's normal (after a repair) and start learning again.
  void relearn() {
    lock();
    st = State();
    saveLocked();
    evaluateLocked();
    unlock();
    events.add("Health: relearning this car's normal");
  }

  // ---- feeding (OBD task)
  void beginDrive(bool hasMaf) {
    if (demo) return;
    lock();
    d = DriveAcc();
    d.active = true;
    d.hasMaf = hasMaf;
    // Battery before the engine started, if read in the last minute.
    if (!isnan(restVolts) && millis() - restVoltsAt < 60000) d.restV = restVolts;
    unlock();
  }

  // Adapter's voltage reading with the engine computer not answering (car off / key on).
  void engineOff(float volts) {
    if (demo || isnan(volts) || volts < 8) return;
    restVolts = volts;
    restVoltsAt = millis();
  }

  // Every polling round: speed, RPM, measured airflow (g/s, NAN without a MAF) and fuel flow (gal/h).
  void fast(float dt, float mph, float rpm, float maf, float gph) {
    if (!d.active || isnan(mph) || isnan(rpm)) return;
    lock();
    d.sec += dt;
    d.lastRpm = rpm;
    if (mph > 5) d.movingSec += dt;
    pushMph(mph);
    bool warm = d.coolant >= WARM_F;

    // Stall: RPM falls to nothing while the engine computer keeps answering,
    // then the engine is started again (turning the key off ends the session instead).
    if (rpm > 400) {
      if (d.stallSeen) {
        noteEvent(E_STALL, "Engine stalled and was restarted");
        d.stallSeen = false;
      }
      d.running = true;
    } else if (rpm < 150 && d.running && mph < 3) {
      d.stallSeen = true;
      d.running = false;
    }

    // Warm idle: RPM steadiness, airflow.
    bool idle = warm && mph < 1 && rpm > 300 && rpm < 1200;
    if (idle) {
      d.idleWin.push_back(rpm);
      d.a[M_IDLE_RPM].add(rpm);
      if (!isnan(maf)) d.a[M_IDLE_MAF].add(maf);
      if (rpm < 450) noteEvent(E_DIP, "Idle RPM dipped near stalling");
      if (d.idleWin.size() >= 40) flushIdle();
    } else {
      flushIdle();
    }

    // Highway: fuel economy, and the RPM-to-speed ratio while holding a steady speed.
    if (mph >= 45 && mph <= 75 && !isnan(gph)) {
      d.hwyMiles += mph * dt / 3600;
      d.hwyGal += gph * dt / 3600;
    }
    if (steady(mph) && mph >= 45 && mph <= 80 && rpm > 600) {
      d.ratioWin.push_back(rpm / mph);
      if (d.ratioWin.size() >= 30) flushRatio();
    } else {
      flushRatio();
    }

    // Thermostat: cooling down while cruising, running hot, never warming up.
    if (d.warmed && mph > 40 && d.coolant < 165) {
      if ((d.coldHwySec += dt) > 90) noteEvent(E_COLD_HWY, "Coolant dropped below 165F on the highway");
    } else {
      d.coldHwySec = 0;
    }
    if (d.coolant >= 225) {
      if ((d.hotSec += dt) > 60) noteEvent(E_HOT, "Coolant stayed above 225F for a minute");
    } else {
      d.hotSec = 0;
    }
    if (d.coldStart && !d.warmed && d.movingSec > 20 * 60) noteEvent(E_NO_WARM, "Engine not warm after 20 min of driving");
    unlock();
  }

  // Every few seconds: temperatures, load and voltage (NAN = not read this time).
  void slow(float coolant, float intake, float load, float volts) {
    if (!d.active) return;
    lock();
    if (!isnan(coolant)) {
      if (coolant <= -35 || coolant >= 275) noteEvent(E_COOLANT_GLITCH, "Coolant sensor reading impossible");
      else if (!isnan(d.coolant) && d.sec - d.coolantAt < 5 && fabsf(coolant - d.coolant) > 25)
        noteEvent(E_COOLANT_GLITCH, "Coolant temperature jumped instantly");
      if (isnan(d.coolant) && coolant < 100) {  // first reading: a cold start
        d.coldStart = true;
        d.coldAt = d.sec;
      }
      if (coolant >= WARM_F && !d.warmed) {
        d.warmed = true;
        if (d.coldStart) d.a[M_WARMUP].add((d.sec - d.coldAt) / 60);
      }
      d.coolant = coolant;
      d.coolantAt = d.sec;
    }
    if (!isnan(intake)) {
      if (intake <= -35 || intake >= 250) noteEvent(E_IAT_GLITCH, "Intake air sensor reading impossible");
      else if (!isnan(d.intake) && d.sec - d.intakeAt < 5 && fabsf(intake - d.intake) > 40) noteEvent(E_IAT_GLITCH, "Intake air temperature jumped instantly");
      d.intake = intake;
      d.intakeAt = d.sec;
    }
    if (!isnan(load)) d.load = load;
    if (!isnan(volts) && volts > 8 && d.lastRpm > 1500) d.a[M_RUN_V].add(volts);
    unlock();
  }

  // Detail readings: fuel trims per bank (%), timing (deg), throttle (%), with the current speed and RPM.
  void details(const float stft[2], const float ltft[2], float timing, float throttle, float mph, float rpm) {
    if (!d.active) return;
    lock();
    if (d.coolant >= WARM_F && !isnan(mph) && !isnan(rpm)) {
      bool idle = mph < 1 && rpm < 1200;
      bool cruise = mph >= 35 && mph <= 80 && steady(mph) && (isnan(d.load) || d.load < 65);
      bool loaded = !isnan(d.load) && d.load >= 65 && mph > 10;
      for (int b = 0; b < 2; b++) {
        float t = isnan(ltft[b]) ? stft[b] : isnan(stft[b]) ? ltft[b] : stft[b] + ltft[b];
        if (isnan(t)) continue;
        if (idle) d.a[M_TRIM_IDLE1 + b].add(t);
        else if (loaded) d.a[M_TRIM_LOAD1 + b].add(t);
        else if (cruise) d.a[M_TRIM_CRUISE1 + b].add(t);
      }
      if (cruise && !isnan(timing) && !isnan(d.load) && d.load >= 20) d.a[M_CRUISE_TIMING].add(timing);
      if (idle && !isnan(throttle)) {
        float closed = !isnan(st.m[M_IDLE_TPS].recent) ? st.m[M_IDLE_TPS].recent : NAN;
        if (!isnan(closed) && throttle > closed + 15) noteEvent(E_TPS_GLITCH, "Throttle reading jumped at idle");
        else d.a[M_IDLE_TPS].add(throttle);
      }
    }
    unlock();
  }

  // The drive is over: fold its averages in, update the checks, save.
  void endDrive() {
    lock();
    if (!d.active) { unlock(); return; }
    d.active = false;
    flushIdle();
    flushRatio();
    bool counts = d.sec > 120;  // a few seconds of engine running teaches nothing
    if (counts) {
      static const uint8_t MIN_N[M_COUNT] = {2, 2, 2, 2, 1, 1, 20, 1, 20, 2, 3, 1, 1, 1, 1, 1, 5};
      for (int i = 0; i < M_COUNT; i++) {
        if (i == M_CRUISE_RATIO || i == M_RATIO_JITTER || i == M_HWY_MPG || i == M_REST_V) continue;
        if (d.a[i].n >= MIN_N[i]) addMetric(i, d.a[i].sum / d.a[i].n);
      }
      // The car's top gear: the lowest RPM-per-mph windows of the drive.
      if (!d.ratios.empty()) {
        float lo = 1e9;
        for (auto &w : d.ratios) lo = min(lo, w.mean);
        float sum = 0, jit = 0;
        int n = 0;
        for (auto &w : d.ratios)
          if (w.mean <= lo * 1.04f) sum += w.mean, jit += w.jitter, n++;
        if (n >= 2) {
          addMetric(M_CRUISE_RATIO, sum / n);
          addMetric(M_RATIO_JITTER, jit / n);
        }
      }
      if (d.hwyMiles >= 3 && d.hwyGal > 0.02f) addMetric(M_HWY_MPG, d.hwyMiles / d.hwyGal);
      if (!isnan(d.restV)) addMetric(M_REST_V, d.restV);
      st.drives++;
      if (d.hasMaf) st.hasMaf = 1;
    }
    // One-off events fade over clean drives.
    for (int e = 0; e < E_COUNT; e++) st.score[e] = st.score[e] * (counts ? 0.8f : 1.0f) + min<int>(d.events[e], 2);
    evaluateLocked(true);
    saveLocked();
    unlock();
  }

  // ---- reading (UI / web)
  std::vector<Result> results() {
    lock();
    std::vector<Result> r(res, res + C_COUNT);
    unlock();
    return r;
  }
  // Counts and the worst check, for summaries.
  struct Overview {
    int check = 0, watch = 0, ok = 0, worst = -1;
    Status worstStatus = NOT_APPLICABLE;
    String worstSummary;
  };
  Overview overview() {
    Overview o;
    lock();
    for (int i = 0; i < C_COUNT; i++) {
      Status s = res[i].status;
      o.check += s == CHECK;
      o.watch += s == WATCH;
      o.ok += s == OK;
      if (s > o.worstStatus) o.worstStatus = s, o.worst = i, o.worstSummary = res[i].summary;
    }
    unlock();
    return o;
  }

  Result result(int c) {
    lock();
    Result r = res[c];
    unlock();
    return r;
  }
  uint16_t drives() const { return st.drives; }
  // Drives until the learned-normal checks are ready (0 = learned).
  int learningLeft() const { return max(0, LEARN_DRIVES - (int)st.drives); }

  // The raw saved state, for backups.
  std::vector<uint8_t> raw() {
    lock();
    std::vector<uint8_t> v((const uint8_t *)&st, (const uint8_t *)&st + sizeof(st));
    unlock();
    return v;
  }
private:
  struct State {
    uint32_t magic = MAGIC;
    uint16_t version = 1, drives = 0;
    HealthMetric m[M_COUNT];
    float score[E_COUNT] = {};
    uint8_t status[C_COUNT] = {};  // last reported, to log changes
    uint8_t hasMaf = 0, pad[2] = {};
  };

public:
  static constexpr size_t STATE_SIZE = sizeof(State);  // the saved file (and its backup)

private:
  static constexpr uint32_t MAGIC = 0x48454C54;  // "HELT"
  static constexpr const char *PATH = "/health.bin";

  struct Acc {
    double sum = 0;
    uint32_t n = 0;
    void add(float v) {
      if (isnan(v)) return;
      sum += v;
      n++;
    }
  };
  struct RatioWin {
    float mean, jitter;  // jitter: spread of the ratio within the window, % of the mean
  };
  struct DriveAcc {
    bool active = false, hasMaf = false;
    float sec = 0, movingSec = 0;
    Acc a[M_COUNT];
    std::vector<float> idleWin, ratioWin;
    std::vector<RatioWin> ratios;
    float mphRing[8] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
    uint8_t ringAt = 0;
    float coolant = NAN, intake = NAN, load = NAN, lastRpm = NAN;
    bool coldStart = false, warmed = false, running = false, stallSeen = false;
    float coldAt = 0, coldHwySec = 0, hotSec = 0, coolantAt = 0, intakeAt = 0;
    float hwyMiles = 0, hwyGal = 0;
    float restV = NAN;
    uint8_t events[E_COUNT] = {};
  };

  SemaphoreHandle_t mtx = nullptr;
  State st;
  DriveAcc d;
  Result res[C_COUNT];
  volatile bool demo = false;
  float displacement = 3.5f;
  float restVolts = NAN;
  uint32_t restVoltsAt = 0;

  void lock() { if (mtx) xSemaphoreTake(mtx, portMAX_DELAY); }
  void unlock() { if (mtx) xSemaphoreGive(mtx); }

  void load() {
    File f = FFat.open(PATH, FILE_READ);
    State s;
    if (f && f.size() == sizeof(State) && f.read((uint8_t *)&s, sizeof(s)) == sizeof(s) && s.magic == MAGIC && s.version == 1)
      st = s;
    if (f) f.close();
  }
  void saveLocked() {
    File f = FFat.open(PATH, FILE_WRITE);
    if (!f) return;
    f.write((const uint8_t *)&st, sizeof(st));
    f.close();
  }

  void addMetric(int i, float v) {
    HealthMetric &m = st.m[i];
    m.n++;
    if (m.n <= LEARN_DRIVES) m.base = isnan(m.base) ? v : m.base + (v - m.base) / m.n;
    m.recent = isnan(m.recent) ? v : m.recent + (v - m.recent) * 0.35f;
  }

  void noteEvent(Event e, const char *what) {
    if (d.events[e] == 0) events.add("Health: %s", what);  // once per drive in the log
    if (d.events[e] < 255) d.events[e]++;
  }

  void pushMph(float v) {
    d.mphRing[d.ringAt] = v;
    d.ringAt = (d.ringAt + 1) % 8;
  }
  // Speed held within 1.5 mph over the last several readings.
  bool steady(float mph) const {
    for (float v : d.mphRing)
      if (isnan(v) || fabsf(v - mph) > 1.5f) return false;
    return true;
  }

  static void meanSd(const std::vector<float> &v, float &mean, float &sd) {
    double s = 0, s2 = 0;
    for (float x : v) s += x, s2 += (double)x * x;
    mean = s / v.size();
    sd = sqrt(max(0.0, s2 / v.size() - (double)mean * mean));
  }
  void flushIdle() {
    if (d.idleWin.size() >= 12) {
      float mean, sd;
      meanSd(d.idleWin, mean, sd);
      d.a[M_IDLE_SD].add(sd);
    }
    d.idleWin.clear();
  }
  void flushRatio() {
    if (d.ratioWin.size() >= 12 && d.ratios.size() < 40) {
      float mean, sd;
      meanSd(d.ratioWin, mean, sd);
      d.ratios.push_back({mean, mean > 0 ? sd / mean * 100 : 0});
    }
    d.ratioWin.clear();
  }

  // ---- the checks
  const HealthMetric &m(int i) const { return st.m[i]; }
  // Recent value once at least two drives have it (one odd drive isn't a trend).
  float recent(int i) const { return m(i).n >= 2 ? m(i).recent : NAN; }
  bool learned(int i) const { return m(i).n >= LEARN_DRIVES; }
  static float avg2(float a, float b) { return isnan(a) ? b : isnan(b) ? a : (a + b) / 2; }
  static String pct(float v) { return (v >= 0 ? "+" : "") + String(v, 0) + "%"; }

  static void set(Result &r, Status s, const String &sum, const String &det) {
    r.status = s;
    r.summary = sum;
    r.detail = det;
  }
  // Fixed-limit severity for a fuel trim: 10% = watch, 15% = check.
  static Status trimLevel(float t) { return fabsf(t) >= 15 ? CHECK : fabsf(t) >= 10 ? WATCH : OK; }
  String learningText(int i) const {
    return "learning (" + String(min<int>(m(i).n, LEARN_DRIVES)) + " of " + String(LEARN_DRIVES) + ")";
  }

  void evaluateLocked(bool logChanges = false) {
    float ti = avg2(recent(M_TRIM_IDLE1), recent(M_TRIM_IDLE2));
    float tc = avg2(recent(M_TRIM_CRUISE1), recent(M_TRIM_CRUISE2));
    float tl = avg2(recent(M_TRIM_LOAD1), recent(M_TRIM_LOAD2));
    bool leakPattern = !isnan(ti) && ti >= 10 && (isnan(tc) || ti - tc >= 6);
    bool loadPattern = !isnan(tl) && tl >= 10 && (isnan(tc) || tl >= tc + 4);
    bool hasMaf = st.hasMaf || d.hasMaf;
    const char *trimNote = "\n\nFuel trims are how much fuel the engine computer adds (+) or takes away (-) to keep the mixture "
                           "right. Within about 10% either way is normal.";

    // Air leaks (vacuum leak): lean at idle, much better at speed.
    Result &leak = res[C_AIR_LEAK];
    if (isnan(ti)) set(leak, NEEDS_DATA, "needs a warm idle", String("Idle with a warm engine for a minute on a couple of drives.") + trimNote);
    else if (leakPattern)
      set(leak, trimLevel(ti), "lean at idle " + pct(ti),
          "At idle the engine computer adds " + pct(ti) + " fuel, but " + (isnan(tc) ? String("no highway data yet") : pct(tc) + " at speed") +
              ". Air sneaking in past the airflow sensor matters most at idle, so this points to a vacuum leak: a cracked "
              "intake hose, the PCV valve or its hose, the brake booster hose, or an intake gasket. A smoke test finds "
              "leaks quickly." + trimNote);
    else if (learned(M_TRIM_IDLE1) && ti - avg2(m(M_TRIM_IDLE1).base, m(M_TRIM_IDLE2).base) >= 8)
      set(leak, WATCH, "idle trims up " + pct(ti - avg2(m(M_TRIM_IDLE1).base, m(M_TRIM_IDLE2).base)),
          "Idle fuel trims are " + pct(ti) + ", up from this car's normal. Not a problem yet, but an early sign of a "
          "small vacuum leak (hoses, PCV valve)." + trimNote);
    else set(leak, OK, "idle trims " + pct(ti), String("No sign of an air leak.") + trimNote);

    // Airflow sensor (MAF): lean at speed too, or idle airflow off.
    Result &maf = res[C_MAF];
    float idleMaf = recent(M_IDLE_MAF);
    if (!hasMaf) set(maf, NOT_APPLICABLE, "", "");
    else if (!isnan(tc) && tc >= 10 && !leakPattern && !loadPattern)
      set(maf, trimLevel(tc), "lean at speed " + pct(tc),
          "At steady speeds the engine computer adds " + pct(tc) + " fuel" + (isnan(ti) ? String("") : ", and " + pct(ti) + " at idle") +
              ". A dirty airflow (MAF) sensor reads less air than is really going in, which looks like this. Try MAF "
              "sensor cleaner spray (never touch the wires) and check the air filter and the intake duct after the "
              "sensor for cracks. A weak fuel pump is the other suspect." + trimNote);
    else if (!isnan(idleMaf) && (idleMaf < displacement * 0.6f || idleMaf > displacement * 2.0f))
      set(maf, WATCH, "idle airflow " + String(idleMaf, 1) + " g/s",
          "At a warm idle the airflow sensor reads " + String(idleMaf, 1) + " g/s; a healthy " + String(displacement, 1) +
              " L engine idles at roughly " + String(displacement * 0.8f, 0) + "-" + String(displacement * 1.6f, 0) +
              " g/s. The sensor may be dirty or failing.");
    else if (learned(M_IDLE_MAF) && !isnan(idleMaf) && idleMaf < m(M_IDLE_MAF).base * 0.8f)
      set(maf, WATCH, "idle airflow down " + String((1 - idleMaf / m(M_IDLE_MAF).base) * 100, 0) + "%",
          "The airflow sensor reads " + String(idleMaf, 1) + " g/s at idle, down from this car's normal " +
              String(m(M_IDLE_MAF).base, 1) + " g/s. A dirty sensor reads low; MAF sensor cleaner usually fixes it.");
    else if (isnan(tc) && isnan(idleMaf)) set(maf, NEEDS_DATA, "needs a drive", "Needs a drive with some steady cruising and a warm idle.");
    else set(maf, OK, isnan(tc) ? String("looks normal") : "trims at speed " + pct(tc), "Airflow readings look normal.");

    // Fuel delivery: lean that gets worse under load.
    Result &fuel = res[C_FUEL];
    if (isnan(tl)) set(fuel, NEEDS_DATA, "needs harder acceleration",
                       "Checked during hard acceleration and hills (high engine load); none seen yet with a warm engine.");
    else if (loadPattern || (!hasMaf && !isnan(tc) && tc >= 10 && !leakPattern))
      set(fuel, trimLevel(max(tl, isnan(tc) ? tl : tc)), "lean under load " + pct(tl),
          "Under load the engine computer adds " + pct(tl) + " fuel" + (isnan(tc) ? String("") : " (" + pct(tc) + " cruising)") +
              ": the engine isn't getting enough fuel when it needs the most. Suspects: a weakening fuel pump, a clogged "
              "fuel filter, or the fuel pressure regulator. A fuel pressure test confirms it." + trimNote);
    else set(fuel, OK, "trims under load " + pct(tl), String("Fuel supply keeps up under load.") + trimNote);

    // Running rich.
    Result &rich = res[C_RICH];
    float lowest = NAN;
    for (float t : {ti, tc, tl})
      if (!isnan(t) && (isnan(lowest) || t < lowest)) lowest = t;
    if (isnan(lowest)) set(rich, NEEDS_DATA, "needs a warm drive", "Needs a drive with a warm engine.");
    else if (lowest <= -10)
      set(rich, trimLevel(lowest), "rich " + pct(lowest),
          "The engine computer is taking away " + pct(-lowest).substring(1) + " fuel: too much is getting in. Suspects: "
          "a leaking injector, the EVAP purge valve stuck open, high fuel pressure (regulator), or fuel diluting the oil "
          "(smell the dipstick)." + trimNote);
    else set(rich, OK, "not rich", String("No sign of too much fuel.") + trimNote);

    // Engine sides (banks): one side needing more fuel than the other.
    Result &banks = res[C_BANKS];
    float d1 = NAN, n1 = 0;
    for (int r = 0; r < 2; r++) {
      float b1 = recent(M_TRIM_IDLE1 + r * 2), b2 = recent(M_TRIM_IDLE2 + r * 2);
      if (isnan(b1) || isnan(b2)) continue;
      d1 = (isnan(d1) ? 0 : d1) + (b1 - b2);
      n1++;
    }
    if (n1 > 0) d1 /= n1;
    if (isnan(d1)) set(banks, isnan(recent(M_TRIM_IDLE1)) && isnan(recent(M_TRIM_CRUISE1)) ? NEEDS_DATA : NOT_APPLICABLE,
                       "needs a warm drive", "Compares the fuel trims of the two sides (banks) of the V6.");
    else if (fabsf(d1) >= 10) {
      int lean = d1 > 0 ? 1 : 2;
      set(banks, fabsf(d1) >= 15 ? CHECK : WATCH, "bank " + String(lean) + " leaner by " + String(fabsf(d1), 0) + "%",
          "Bank " + String(lean) + (lean == 1 ? " (the side with cylinder 1)" : " (the side without cylinder 1)") +
              " needs " + String(fabsf(d1), 0) + "% more fuel than the other. Something affects that side only: a vacuum "
              "leak or intake gasket on that side, a clogged injector, its O2 sensor, or an exhaust leak ahead of that "
              "sensor." + trimNote);
    } else set(banks, OK, "within " + String(fabsf(d1), 0) + "%", "Both sides of the engine need about the same fuel correction.");

    // Idle quality (IAC valve / throttle body).
    Result &idle = res[C_IDLE];
    float rpm = recent(M_IDLE_RPM), sd = recent(M_IDLE_SD);
    const char *idleSuspects = " Suspects: a dirty throttle body or idle air control (IAC) valve - cleaning them is the "
                               "usual fix on this engine - a vacuum leak, or a weak ignition coil.";
    if (st.score[E_STALL] >= 0.5f)
      set(idle, CHECK, "stalled at idle", String("The engine stalled while stopped and had to be restarted.") + idleSuspects);
    else if (isnan(rpm) || isnan(sd)) set(idle, NEEDS_DATA, "needs a warm idle", "Idle with a warm engine for a minute on a couple of drives.");
    else if (sd >= 100 || (sd >= 50 && (!learned(M_IDLE_SD) || sd >= m(M_IDLE_SD).base * 1.8f)))
      set(idle, sd >= 100 ? CHECK : WATCH, "hunting +-" + String(sd, 0) + " rpm",
          "The idle speed wanders by about " + String(sd, 0) + " RPM instead of holding steady." + idleSuspects);
    else if (st.score[E_DIP] >= 0.5f)
      set(idle, WATCH, "dips near stalling", String("The idle dropped below 450 RPM, close to stalling.") + idleSuspects);
    else if (rpm < 550)
      set(idle, WATCH, "low " + String(rpm, 0) + " rpm", "A warm idle of " + String(rpm, 0) + " RPM is low." + idleSuspects);
    else if (learned(M_IDLE_RPM) && fabsf(rpm - m(M_IDLE_RPM).base) >= 150)
      set(idle, WATCH, String(rpm, 0) + " rpm (normal " + String(m(M_IDLE_RPM).base, 0) + ")",
          "The warm idle speed has moved from this car's normal " + String(m(M_IDLE_RPM).base, 0) + " RPM to " +
              String(rpm, 0) + "." + idleSuspects);
    else set(idle, OK, String(rpm, 0) + " rpm, steady", "Idle speed is steady (wanders +-" + String(sd, 0) + " RPM).");

    // Throttle position sensor.
    Result &thr = res[C_THROTTLE];
    float tps = recent(M_IDLE_TPS);
    if (st.score[E_TPS_GLITCH] >= 0.5f)
      set(thr, st.score[E_TPS_GLITCH] >= 1.5f ? CHECK : WATCH, "reading jumps",
          "The throttle position reading jumped while idling with the pedal up. A worn throttle position sensor does this "
          "and can cause surging or odd shifting.");
    else if (isnan(tps)) set(thr, NEEDS_DATA, "needs a warm idle", "Needs a warm idle on a couple of drives.");
    else if (learned(M_IDLE_TPS) && fabsf(tps - m(M_IDLE_TPS).base) >= 4)
      set(thr, WATCH, String(tps, 0) + "% closed (normal " + String(m(M_IDLE_TPS).base, 0) + "%)",
          "With the pedal up the throttle reads " + String(tps, 0) + "%, versus this car's normal " +
              String(m(M_IDLE_TPS).base, 0) + "%. Carbon on the throttle plate or a worn sensor.");
    else set(thr, learned(M_IDLE_TPS) ? OK : LEARNING, learned(M_IDLE_TPS) ? String("steady") : learningText(M_IDLE_TPS),
             "The throttle reading at idle is " + String(tps, 0) + "% and steady.");

    // Thermostat and cooling.
    Result &th = res[C_THERMOSTAT];
    float wu = recent(M_WARMUP);
    if (st.score[E_COLD_HWY] >= 0.5f || st.score[E_NO_WARM] >= 0.5f)
      set(th, CHECK, st.score[E_COLD_HWY] >= 0.5f ? "cools on the highway" : "slow to warm up",
          "The engine doesn't stay at operating temperature (it should hold about 190-210F once warm). A thermostat stuck "
          "open is the usual cause: worse MPG, weak cabin heat, and eventually a P0128 code. It's an inexpensive part.");
    else if (st.score[E_HOT] >= 0.5f)
      set(th, WATCH, "ran hot",
          "Coolant stayed above 225F for over a minute. Check the coolant level (engine cold), the radiator fan, and "
          "for a thermostat sticking closed.");
    else if (!isnan(wu) && learned(M_WARMUP) && wu > m(M_WARMUP).base * 1.5f && wu > 10)
      set(th, WATCH, "warms in " + String(wu, 0) + " min (normal " + String(m(M_WARMUP).base, 0) + ")",
          "Warming up takes longer than this car's normal. Cold weather does this too; if it continues in mild "
          "weather, the thermostat may be opening early.");
    else if (isnan(m(M_WARMUP).recent)) set(th, NEEDS_DATA, "needs a cold start", "Checked from a cold start (car sat a few hours).");
    else set(th, OK, "warms in " + String(m(M_WARMUP).recent, 0) + " min", "Warms up and holds temperature normally.");

    // Temperature sensors.
    Result &ts = res[C_TEMP_SENSORS];
    float cg = st.score[E_COOLANT_GLITCH], ig = st.score[E_IAT_GLITCH];
    if (cg >= 0.5f || ig >= 0.5f) {
      String which = cg >= ig ? "coolant temperature" : "intake air temperature";
      set(ts, max(cg, ig) >= 1.5f ? CHECK : WATCH, cg >= ig ? "coolant sensor glitches" : "intake sensor glitches",
          "The " + which + " reading jumped by an impossible amount or read out of range. A failing sensor or a loose, "
          "corroded connector. A bad coolant sensor also upsets fuel mixture and the temperature gauge.");
    } else set(ts, st.drives ? OK : NEEDS_DATA, st.drives ? String("readings plausible") : String("needs a drive"),
               "Coolant and intake air temperature readings change smoothly and stay in range.");

    // Knock (ignition timing pulled back).
    Result &kn = res[C_KNOCK];
    float tim = recent(M_CRUISE_TIMING);
    if (isnan(m(M_CRUISE_TIMING).recent)) set(kn, NEEDS_DATA, "needs steady cruising", "Needs some steady cruising with a warm engine.");
    else if (!learned(M_CRUISE_TIMING)) set(kn, LEARNING, learningText(M_CRUISE_TIMING), "Learning this car's normal ignition timing at cruise.");
    else if (!isnan(tim) && tim < m(M_CRUISE_TIMING).base - 4)
      set(kn, WATCH, String(m(M_CRUISE_TIMING).base - tim, 0) + " deg less timing",
          "At cruise the ignition timing is " + String(tim, 0) + " deg, down from this car's normal " +
              String(m(M_CRUISE_TIMING).base, 0) + ". The engine computer pulls timing back when it hears knock. Try "
              "a different gas station or higher octane; carbon buildup and an engine running hot also cause it.");
    else set(kn, OK, "timing normal", "Ignition timing at cruise matches this car's normal: no sign of knock.");

    // Transmission: RPM at highway speed, and steadiness.
    Result &tr = res[C_TRANS];
    float ratio = recent(M_CRUISE_RATIO), jit = recent(M_RATIO_JITTER);
    if (isnan(m(M_CRUISE_RATIO).recent)) set(tr, NEEDS_DATA, "needs highway driving", "Needs a steady 45+ mph stretch.");
    else if (!learned(M_CRUISE_RATIO)) set(tr, LEARNING, learningText(M_CRUISE_RATIO), "Learning this car's RPM at highway speed.");
    else if (!isnan(ratio) && ratio > m(M_CRUISE_RATIO).base * 1.06f)
      set(tr, WATCH, "RPM " + String((ratio / m(M_CRUISE_RATIO).base - 1) * 100, 0) + "% high at speed",
          "At steady highway speed the engine turns " + String((ratio / m(M_CRUISE_RATIO).base - 1) * 100, 0) +
              "% faster than this car's normal. The torque converter may not be locking up, or the transmission is "
              "slipping. Check the fluid level and condition (should be red, not brown or burnt-smelling).");
    else if (!isnan(jit) && jit > max(2.5f, m(M_RATIO_JITTER).base * 2))
      set(tr, WATCH, "RPM unsteady at speed",
          "At a steady highway speed the RPM wanders more than it used to: possible slipping or torque converter "
          "shudder. Check the fluid level and condition.");
    else set(tr, OK, "normal", "RPM at highway speed matches this car's normal.");

    // Highway MPG trend.
    Result &mp = res[C_MPG];
    float mpg = recent(M_HWY_MPG);
    if (isnan(m(M_HWY_MPG).recent)) set(mp, NEEDS_DATA, "needs highway driving", "Needs a few miles at 45-75 mph.");
    else if (!learned(M_HWY_MPG)) set(mp, LEARNING, learningText(M_HWY_MPG), "Learning this car's normal highway MPG.");
    else if (!isnan(mpg) && mpg < m(M_HWY_MPG).base * 0.88f)
      set(mp, WATCH, String((1 - mpg / m(M_HWY_MPG).base) * 100, 0) + "% below normal",
          "Highway MPG is " + String(mpg, 1) + ", down from this car's normal " + String(m(M_HWY_MPG).base, 1) +
              ". Check tire pressures, a dragging brake (one wheel hotter than the others after a drive), and the "
              "air filter. Winter fuel and cold weather cost 5-10% on their own.");
    else set(mp, OK, String(m(M_HWY_MPG).recent, 1) + " MPG", "Highway MPG matches this car's normal.");

    // Battery and charging, against this car's own normal (the adapter's voltage reading has an offset).
    Result &bt = res[C_BATTERY];
    float rv = recent(M_REST_V), cv = recent(M_RUN_V);
    if (isnan(m(M_RUN_V).recent)) set(bt, NEEDS_DATA, "needs a drive", "Needs a drive.");
    else if (learned(M_REST_V) && !isnan(rv) && rv < m(M_REST_V).base - 0.35f)
      set(bt, WATCH, "weaker before starting",
          "The battery voltage before starting is " + String(m(M_REST_V).base - rv, 1) + " V lower than this car's "
          "normal. The battery may be wearing out; most parts stores load-test it free.");
    else if (learned(M_RUN_V) && !isnan(cv) && cv < m(M_RUN_V).base - 0.4f)
      set(bt, WATCH, "charging voltage down",
          "While driving the voltage is " + String(m(M_RUN_V).base - cv, 1) + " V below this car's normal: a slipping "
          "belt or a tiring alternator.");
    else if (learned(M_RUN_V) && !isnan(cv) && cv > m(M_RUN_V).base + 0.5f)
      set(bt, WATCH, "charging voltage up",
          "While driving the voltage is " + String(cv - m(M_RUN_V).base, 1) + " V above this car's normal: the "
          "alternator's voltage regulator may be failing, which can cook the battery.");
    else if (!learned(M_RUN_V)) set(bt, LEARNING, learningText(M_RUN_V), "Learning this car's normal battery and charging voltage.");
    else set(bt, OK, "normal", "Battery and charging voltages match this car's normal.");

    if (logChanges) {
      for (int c = 0; c < C_COUNT; c++) {
        Status now = res[c].status, was = (Status)st.status[c];
        bool bad = now >= WATCH, wasBad = was >= WATCH;
        if (now != was && (bad || wasBad))
          events.add("Health: %s now %s (%s)", checkName(c), statusName(now), res[c].summary.c_str());
        st.status[c] = now;
      }
    }
  }
};
