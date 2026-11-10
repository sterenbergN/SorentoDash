// The simulated board behind the Arduino stand-ins: knob, button, touch,
// backlight, LED ring, USB console and an emulated Adafruit Audio FX board
// (same serial protocol; plays WAVs from sim/sounds through SDL).
#include "SimHardware.h"
#include "SimState.h"
#include "shim/Arduino.h"
#include <SDL.h>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace sim {

State state;

static uint32_t nowMs() {
  static const auto t0 = std::chrono::steady_clock::now();
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------- pins & interrupts
constexpr int PIN_A = 45, PIN_B = 42, PIN_SW = 41, PIN_BACKLIGHT = 46;
static std::map<int, std::pair<Isr, void *>> isrs;

int readPin(int pin) {
  if (pin == PIN_A) return state.encA;
  if (pin == PIN_B) return state.encB;
  if (pin == PIN_SW) return state.buttonDown ? 0 : 1;  // pulled up, pressed = LOW
  return 1;
}
void writePin(int, int) {}
void attachIsr(int pin, Isr fn, void *arg) { isrs[pin] = {fn, arg}; }

static void edge(int pin) {
  auto it = isrs.find(pin);
  if (it != isrs.end()) it->second.first(it->second.second);
}

// One knob detent = four quadrature edges, fed through the firmware's own ISR.
void turnKnob(int detents) {
  static const int cw[4][2] = {{1, 0}, {1, 1}, {0, 1}, {0, 0}};   // A,B after each edge
  static const int ccw[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  const int(*seq)[2] = detents > 0 ? cw : ccw;
  for (int d = 0; d < std::abs(detents); d++) {
    for (int i = 0; i < 4; i++) {
      int changed = seq[i][0] != state.encA ? PIN_A : PIN_B;
      state.encA = seq[i][0];
      state.encB = seq[i][1];
      edge(changed);
    }
  }
}

void setPwm(int pin, uint32_t v) {
  if (pin == PIN_BACKLIGHT) state.backlight = v;
}

void setLeds(const uint32_t *rgb, int n) {
  std::lock_guard<std::mutex> l(state.ledMutex);
  state.leds.assign(rgb, rgb + n);
}

// ---------------------------------------------------------------- touch (CST816D)
int touchRead(uint8_t reg, uint8_t *buf, int n) {
  uint8_t regs[16] = {};
  int x = 0, y = 0;
  bool down = state.touch.at(nowMs(), x, y);
  regs[0x02] = down ? 1 : 0;
  regs[0x03] = (x >> 8) & 0x0F;
  regs[0x04] = x & 0xFF;
  regs[0x05] = (y >> 8) & 0x0F;
  regs[0x06] = y & 0xFF;
  for (int i = 0; i < n; i++) buf[i] = reg + i < 16 ? regs[reg + i] : 0;
  return n;
}

bool TouchSim::at(uint32_t t, int &x, int &y) {
  std::lock_guard<std::mutex> l(m);
  if (dragging) { x = dx; y = dy; return true; }
  if (!scripted || t < start) return false;
  uint32_t e = t - start;
  if (e > 140) { scripted = false; return false; }
  float f = std::min(1.0f, e / 110.0f);
  x = (int)(x0 + (x1 - x0) * f);
  y = 120;
  return true;
}

void TouchSim::swipe(int dir) {
  std::lock_guard<std::mutex> l(m);
  scripted = true;
  start = nowMs();
  x0 = dir > 0 ? 190 : 50;  // swipe left = finger moves right->left
  x1 = dir > 0 ? 50 : 190;
}

void TouchSim::drag(bool on, int x, int y) {
  std::lock_guard<std::mutex> l(m);
  dragging = on;
  dx = std::max(0, std::min(239, x));
  dy = std::max(0, std::min(239, y));
}

// ---------------------------------------------------------------- console
static std::mutex consoleMutex;
static std::deque<uint8_t> consoleIn;

void consoleWrite(const uint8_t *buf, size_t n) {
  fwrite(buf, 1, n, stdout);
  fflush(stdout);
}
int consoleAvailable() {
  std::lock_guard<std::mutex> l(consoleMutex);
  return (int)consoleIn.size();
}
int consoleRead() {
  std::lock_guard<std::mutex> l(consoleMutex);
  if (consoleIn.empty()) return -1;
  int c = consoleIn.front();
  consoleIn.pop_front();
  return c;
}
static std::deque<std::string> simCommands;

bool nextSimCommand(std::string &out) {
  std::lock_guard<std::mutex> l(consoleMutex);
  if (simCommands.empty()) return false;
  out = simCommands.front();
  simCommands.pop_front();
  return true;
}

void startConsoleReader() {
  std::thread([] {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == 13) line.pop_back();  // CR from Windows line ends
      std::lock_guard<std::mutex> l(consoleMutex);
      if (!line.empty() && line[0] == '@') {  // for the simulator itself, not the firmware
        simCommands.push_back(line.substr(1));
        continue;
      }
      for (char c : line) consoleIn.push_back((uint8_t)c);
      consoleIn.push_back('\n');
    }
  }).detach();
}

// ---------------------------------------------------------------- paths & web
std::string stateDir() { return state.root + "/state"; }
void noteWebRequest() { state.lastWeb = nowMs(); }
bool recentWebRequest() { return state.lastWeb && nowMs() - state.lastWeb < 20000; }

// ---------------------------------------------------------------- Audio FX emulation
struct Sound {
  std::string name83;           // "TRAIN   WAV"
  std::vector<int16_t> pcm;     // 44.1 kHz mono
};
static std::vector<Sound> sounds;
static std::deque<uint8_t> fxOut;
static std::string fxLine;
static bool fxOpen = false;
static int fxVolume = 204;
static int fxPlaying = -1;
static uint32_t fxStart = 0, fxLength = 0;
static SDL_AudioDeviceID audioDev = 0;

static void fxSay(const std::string &s) { fxOut.insert(fxOut.end(), s.begin(), s.end()); }

static std::string to83(std::string stem, std::string ext) {
  for (auto &c : stem) c = (char)toupper((unsigned char)c);
  for (auto &c : ext) c = (char)toupper((unsigned char)c);
  std::string clean;
  for (char c : stem)
    if (isalnum((unsigned char)c) || c == '_' || c == '-') clean += c;
  clean = clean.substr(0, 8);
  clean.resize(8, ' ');
  return clean + ext.substr(0, 3);
}

// Two-tone horn, for when sim/sounds has no files.
static std::vector<int16_t> synthHorn(float seconds, float f1, float f2) {
  std::vector<int16_t> v((size_t)(seconds * 44100));
  for (size_t i = 0; i < v.size(); i++) {
    float t = i / 44100.0f, env = std::min(1.0f, std::min(t / 0.03f, (seconds - t) / 0.08f));
    float s = 0.35f * (std::sin(2 * 3.14159f * f1 * t) > 0 ? 1.f : -1.f) + 0.35f * (std::sin(2 * 3.14159f * f2 * t) > 0 ? 1.f : -1.f);
    v[i] = (int16_t)(s * env * 9000);
  }
  return v;
}

static bool loadWav(const fs::path &p, std::vector<int16_t> &out) {
  SDL_AudioSpec spec;
  Uint8 *buf = nullptr;
  Uint32 len = 0;
  if (!SDL_LoadWAV(p.string().c_str(), &spec, &buf, &len)) return false;
  SDL_AudioCVT cvt;
  SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS, 1, 44100);
  std::vector<Uint8> work(len * (cvt.len_mult > 0 ? cvt.len_mult : 1) + 16);
  memcpy(work.data(), buf, len);
  SDL_FreeWAV(buf);
  cvt.buf = work.data();
  cvt.len = (int)len;
  if (cvt.needed && SDL_ConvertAudio(&cvt) != 0) return false;
  int outLen = cvt.needed ? cvt.len_cvt : (int)len;
  out.assign((int16_t *)work.data(), (int16_t *)(work.data() + outLen));
  return true;
}

void loadSounds() {
  sounds.clear();
  fs::path dir = fs::path(state.root) / "sounds";
  std::error_code ec;
  if (fs::exists(dir, ec)) {
    std::vector<fs::path> files;
    for (auto &e : fs::directory_iterator(dir, ec)) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (auto &p : files) {
      std::string ext = p.extension().string();
      for (auto &c : ext) c = (char)toupper((unsigned char)c);
      if (ext != ".WAV" && ext != ".OGG") continue;
      Sound s{to83(p.stem().string(), ext.substr(1)), {}};
      if (ext == ".WAV") loadWav(p, s.pcm);
      if (s.pcm.empty()) s.pcm = synthHorn(2.0f, 420, 530);  // OGG isn't decoded here
      sounds.push_back(s);
    }
  }
  if (sounds.empty()) {
    const char *names[] = {"TRAIN", "DIXIE", "AIRHORN", "BIG_RIG", "REVEILLE", "DUCK"};
    float f = 300;
    for (const char *n : names) {
      sounds.push_back({to83(n, "WAV"), synthHorn(1.6f + (f - 300) / 400, f, f * 1.26f)});
      f += 60;
    }
  }
  printf("[sim] sound board: %d files%s\n", (int)sounds.size(),
         fs::exists(dir, ec) ? "" : " (built-in tones; put WAVs in sim/sounds to use your own)");
}

void initAudio() {
  SDL_AudioSpec want{}, have{};
  want.freq = 44100;
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 1024;
  audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (audioDev) SDL_PauseAudioDevice(audioDev, 0);
}

static void fxStop() {
  if (audioDev) SDL_ClearQueuedAudio(audioDev);
  fxPlaying = -1;
}

static void fxPlay(int i) {
  fxStop();
  const Sound &s = sounds[i];
  float gain = std::pow(10.0f, -(204 - fxVolume) * 0.5f / 20.0f);  // 0.5 dB per step
  std::vector<int16_t> scaled(s.pcm.size());
  for (size_t k = 0; k < s.pcm.size(); k++) scaled[k] = (int16_t)(s.pcm[k] * gain);
  if (audioDev) SDL_QueueAudio(audioDev, scaled.data(), (Uint32)(scaled.size() * 2));
  fxPlaying = i;
  fxStart = nowMs();
  fxLength = (uint32_t)(s.pcm.size() * 1000 / 44100);
}

static void fxCommand(const std::string &cmd) {
  char b[96];
  if (cmd == "L") {
    snprintf(b, sizeof(b), "\r\r\nFiles %05d\r\n", (int)sounds.size());
    fxSay(b);
    for (auto &s : sounds) {
      snprintf(b, sizeof(b), "%s\t%010u\t\r\n", s.name83.c_str(), (unsigned)(s.pcm.size() * 2));
      fxSay(b);
    }
  } else if (!cmd.empty() && cmd[0] == '#') {
    int n = atoi(cmd.c_str() + 1);
    if (n < 0 || n >= (int)sounds.size()) { fxSay("\r\nNoFile\r\n"); return; }
    fxPlay(n);
    snprintf(b, sizeof(b), "\r\n\rplay\t%05d\t%s\r\n", n, sounds[n].name83.c_str());
    fxSay(b);
  } else if (cmd == "+" || cmd == "-") {
    fxVolume = std::max(0, std::min(204, fxVolume + (cmd == "+" ? 2 : -2)));
    snprintf(b, sizeof(b), "%05d\r\n", fxVolume);
    fxSay(b);
  }
}

static void fxImmediate(char c) {
  char b[64];
  if (c == 'q') {
    bool was = fxPlaying >= 0;
    fxStop();
    fxSay(was ? "qdone\r\r\n" : "q\r\n");
  } else if (c == 's') {
    uint32_t total = fxPlaying >= 0 ? (uint32_t)sounds[fxPlaying].pcm.size() * 2 : 0;
    uint32_t el = fxPlaying >= 0 ? nowMs() - fxStart : 0;
    uint32_t remain = fxPlaying >= 0 && fxLength ? (uint32_t)(total * (1.0 - std::min(1.0, el / (double)fxLength))) : 0;
    snprintf(b, sizeof(b), "%010u/%010u\r\n", remain, total);
    fxSay(b);
  } else if (c == '=' || c == '>') {
    fxSay(std::string(1, c));
  }
}

void soundboardOpen(bool open) {
  fxOpen = open;
  fxLine.clear();
}

void soundboardWrite(uint8_t c) {
  if (!fxOpen) return;
  if (fxLine.empty() && (c == 'q' || c == 's' || c == '=' || c == '>')) { fxImmediate((char)c); return; }
  if (c == '\r') return;
  if (c == '\n') {
    fxCommand(fxLine);
    fxLine.clear();
  } else {
    fxLine += (char)c;
  }
}

int soundboardAvailable() {
  if (fxPlaying >= 0 && nowMs() - fxStart >= fxLength) {
    fxPlaying = -1;
    fxSay("done\r\r\n");
  }
  return fxOpen ? (int)fxOut.size() : 0;
}

int soundboardRead() {
  if (fxOut.empty()) return -1;
  int c = fxOut.front();
  fxOut.pop_front();
  return c;
}

}  // namespace sim

// Globals the Arduino stand-ins declare.
HWCDC Serial;
HardwareSerial Serial1;
EspClass ESP;
