// Non-blocking driver for the Adafruit Audio FX Sound Board in UART mode.
//
// The board enters UART mode when its "UG" pin is tied to GND at power-up.
// Protocol (9600 8N1, see Adafruit_Soundboard library):
//   "L\n"      -> one line per file: 11-char 8.3 name, TAB, size
//   "#<n>\n"   -> "play\t<n>\t<name>"  or "NoFile"
//   "q"        -> stop
//   "+\n"/"-\n"-> new volume (0..204, in 0.5 dB steps of attenuation)
//   "s"        -> "<remaining>/<total>" bytes of the current track
//   unsolicited "done" when a track finishes.
// Only one command is in flight at a time; the rest wait in a small queue so
// the UI loop never blocks on the 9600 baud link.
#pragma once
#include <Arduino.h>

class AudioFX {
public:
  static constexpr int MAX_FILES = 64;
  static constexpr int VOL_MAX = 204;

  struct File {
    char name[12];  // raw 8.3 name, e.g. "TRAIN   OGG"
    uint32_t size;
  };

  void begin(HardwareSerial &s, int rxPin, int txPin) {
    ser = &s;
    addPins(rxPin, txPin);
    addPins(txPin, rxPin);  // tolerate crossed TX/RX wires
    openPort();
    requestList();
  }

  // Extra RX/TX pin pairs to probe while the board has never answered.
  void addPins(int rx, int tx) {
    if (nPairs < 6) pairs[nPairs][0] = rx, pairs[nPairs][1] = tx, nPairs++;
  }

  bool connected() const { return online; }
  int fileCount() const { return nFiles; }
  const File &file(int i) const { return files[i]; }
  uint32_t listVersion() const { return listVer; }
  int reboots() const { return rebootCount; }
  int rxPin() const { return pairs[curPair][0]; }

  bool isPlaying() const { return playing; }
  int playingTrack() const { return curTrack; }
  float progress() const { return prog; }  // 0..1, or -1 when unknown

  int volume() const { return vol; }  // -1 until known
  // Desired volume; re-applied automatically whenever the board resets.
  void setVolumeTarget(int v) {
    volWant = constrain(v, 0, VOL_MAX);
    volSettled = false;
  }

  void requestList() {
    dropQueued(C_LIST);
    enqueue(C_LIST, 0);
  }

  void play(int track) {
    dropQueued(C_SIZE);
    if (playing) enqueue(C_STOP, 0);
    enqueue(C_PLAY, track);
    prog = 0;
  }

  void stop() {
    dropQueued(C_SIZE);
    dropQueued(C_PLAY);
    enqueue(C_STOP, 0);
  }

  // Mirror all link traffic to a debug stream (e.g. the USB console).
  void setLog(Stream *s) { log = s; }

  // Debug passthrough from the USB console.
  void raw(const String &cmd) {
    if (ser) ser->println(cmd);
  }

  void poll() {
    if (!ser) return;
    uint32_t now = millis();

    if (log && ser->available()) log->print("<< ");
    bool gotBytes = false;
    while (ser->available()) {
      char c = ser->read();
      lastRx = now;
      gotBytes = true;
      if (log) {
        if (c == 0x0A) log->print("\\n");
        else if (c == 0x0D) log->print("\\r");
        else if (c == 0x09) log->print("\\t");
        else if (c < 32 || c > 126) log->printf("[%02X]", (uint8_t)c);
        else log->print(c);
      }
      if (c == '\r') continue;
      if (c == '\n') {
        line[lineLen] = 0;
        if (lineLen) handleLine(now);
        lineLen = 0;
      } else if (lineLen < sizeof(line) - 1) {
        line[lineLen++] = c;
      }
    }
    if (log && gotBytes) log->println();

    // Command completion by timeout.
    if (inFlight != C_NONE) {
      if (inFlight == C_LIST) {
        bool gotLines = listCount > 0;
        if ((gotLines && now - lastRx > 350) || (!gotLines && now - sentAt > 1500)) finishList(gotLines);
      } else if (now - sentAt > 400) {
        if (log) log->printf("-- timeout (cmd %d)\n", inFlight);
        if (inFlight == C_PLAY || inFlight == C_STOP) playing = false;
        inFlight = C_NONE;
        // A few missed replies in a row: check the link with a file listing
        // rather than declaring the board gone on the spot.
        if (++misses >= 3) {
          misses = 0;
          requestList();
        }
      }
    }

    // Re-list shortly after the board announces a reboot.
    if (relistAt && (int32_t)(now - relistAt) >= 0) {
      relistAt = 0;
      requestList();
    }

    // Keep trying to find the board while it is missing (it may boot later).
    if (!online && inFlight == C_NONE && qLen == 0 && now - lastProbe > 3000) {
      lastProbe = now;
      requestList();
    }

    if (inFlight == C_NONE && qLen == 0 && online) {
      if (vol < 0 && !playing) {
        // Learn the current volume without changing it.
        enqueue(C_VOLDN, 0);
        enqueue(C_VOLUP, 0);
      } else if (volWant >= 0 && vol >= 0 && !volSettled) {
        // Walk the volume toward the requested level one step at a time.
        if (vol < volWant - 1) enqueue(C_VOLUP, 1);
        else if (vol > volWant + 1) enqueue(C_VOLDN, 1);
        else volSettled = true;
      } else if (playing && now - lastSizePoll > 200) {
        lastSizePoll = now;
        enqueue(C_SIZE, 0);
      }
    }

    if (inFlight == C_NONE && qLen) sendNext(now);
  }

private:
  enum Cmd : uint8_t { C_NONE, C_LIST, C_PLAY, C_STOP, C_VOLUP, C_VOLDN, C_SIZE };
  struct Req { Cmd cmd; int arg; };

  HardwareSerial *ser = nullptr;
  Stream *log = nullptr;
  int pairs[6][2];
  int nPairs = 0, curPair = 0;

  void openPort() {
    ser->end();
    int rx = pairs[curPair][0], tx = pairs[curPair][1];
    ser->begin(9600, SERIAL_8N1, rx, tx);
    if (log) log->printf("-- UART rx=GPIO%d tx=GPIO%d\n", rx, tx);
  }

  char line[64];
  size_t lineLen = 0;

  Req q[8];
  int qLen = 0;
  Cmd inFlight = C_NONE;
  uint32_t sentAt = 0, lastRx = 0, lastProbe = 0, lastSizePoll = 0, relistAt = 0;
  int misses = 0, failedProbes = 0, rebootCount = 0;
  bool everConnected = false;

  bool online = false;
  File files[MAX_FILES];
  File pending[MAX_FILES];
  int nFiles = 0, listCount = 0;
  uint32_t listVer = 0;

  bool playing = false;
  int curTrack = -1;
  float prog = -1;
  int vol = -1, volWant = -1, lastVolDir = 0;
  bool volSettled = false;

  void enqueue(Cmd c, int arg) {
    if (qLen < 8) q[qLen++] = {c, arg};
  }

  void dropQueued(Cmd c) {
    int j = 0;
    for (int i = 0; i < qLen; i++)
      if (q[i].cmd != c) q[j++] = q[i];
    qLen = j;
  }

  void sendNext(uint32_t now) {
    Req r = q[0];
    for (int i = 1; i < qLen; i++) q[i - 1] = q[i];
    qLen--;

    while (ser->available()) ser->read();
    lineLen = 0;
    inFlight = r.cmd;
    sentAt = now;
    if (log) {
      static const char *names[] = {"", "L", "#", "q", "+", "-", "s"};
      log->printf(">> %s%s\n", names[r.cmd], r.cmd == C_PLAY ? String(r.arg).c_str() : "");
    }
    switch (r.cmd) {
      case C_LIST: listCount = 0; ser->println('L'); break;
      case C_PLAY: curTrack = r.arg; ser->print('#'); ser->println(r.arg); break;
      case C_STOP: ser->print('q'); break;
      case C_VOLUP: lastVolDir = r.arg ? 1 : 0; ser->println('+'); break;
      case C_VOLDN: lastVolDir = r.arg ? -1 : 0; ser->println('-'); break;
      case C_SIZE: ser->print('s'); break;
      default: inFlight = C_NONE; break;
    }
  }

  void complete() {
    inFlight = C_NONE;
    misses = 0;
  }

  void goOffline() {
    online = false;
    playing = false;
    vol = -1;
    volSettled = false;
    misses = 0;
    qLen = 0;
  }

  void finishList(bool gotLines) {
    inFlight = C_NONE;
    if (!gotLines) {
      goOffline();
      // Before the board has ever answered, cycle through the candidate pin
      // pairs. Afterwards only move on if the known-good pair keeps failing.
      if (nPairs > 1 && (!everConnected || ++failedProbes >= 5)) {
        failedProbes = 0;
        curPair = (curPair + 1) % nPairs;
        openPort();
      }
      return;
    }
    memcpy(files, pending, sizeof(File) * listCount);
    nFiles = listCount;
    listVer++;
    misses = 0;
    failedProbes = 0;
    online = true;
    everConnected = true;
  }

  static bool isDigits(const char *s) {
    if (!*s) return false;
    for (; *s; s++)
      if (!isdigit((unsigned char)*s)) return false;
    return true;
  }

  void handleLine(uint32_t now) {
    // "done" can arrive glued to a command echo, e.g. "qdone".
    if (strstr(line, "done") && inFlight != C_LIST) {
      playing = false;
      prog = 1;
      if (inFlight == C_SIZE || inFlight == C_STOP) complete();
      return;
    }
    if (strstr(line, "Adafruit FX Sound Board")) {
      // Board (re)booted: its volume reset and it needs a moment to mount.
      rebootCount++;
      playing = false;
      vol = -1;
      volSettled = false;
      qLen = 0;
      inFlight = C_NONE;
      relistAt = now + 1500;
      return;
    }

    switch (inFlight) {
      case C_LIST:
        // "NAME    EXT\t0001234"
        if (strlen(line) >= 13 && line[11] == '\t' && listCount < MAX_FILES) {
          File &f = pending[listCount++];
          memcpy(f.name, line, 11);
          f.name[11] = 0;
          f.size = strtoul(line + 12, nullptr, 10);
        }
        break;
      case C_PLAY:
        if (strncmp(line, "play", 4) == 0) {
          curTrack = atoi(line + 5);
          playing = true;
          prog = 0;
          complete();
        } else if (strstr(line, "NoFile")) {
          playing = false;
          complete();
        }
        break;
      case C_STOP:
        playing = false;
        complete();
        break;
      case C_VOLUP:
      case C_VOLDN:
        if (isDigits(line)) {
          int v = atoi(line);
          if (lastVolDir != 0 && vol >= 0) {
            // Stop stepping on overshoot or at the end stop.
            if ((lastVolDir > 0 && v > volWant) || (lastVolDir < 0 && v < volWant) || v == vol) volSettled = true;
          }
          vol = v;
          complete();
        }
        break;
      case C_SIZE: {
        const char *slash = strchr(line, '/');
        if (slash) {
          uint32_t remain = strtoul(line, nullptr, 10);
          uint32_t total = strtoul(slash + 1, nullptr, 10);
          if (total) prog = 1.0f - (float)remain / total;
          complete();
        }
        break;
      }
      default:
        break;
    }
  }
};
