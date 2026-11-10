// Shared state between the simulator window (main.cpp) and the simulated
// hardware (SimHardware.cpp).
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sim {

struct TouchSim {
  std::mutex m;
  bool scripted = false, dragging = false;
  uint32_t start = 0;
  int x0 = 0, x1 = 0, dx = 0, dy = 0;
  bool at(uint32_t t, int &x, int &y);  // current finger, if any
  void swipe(int dir);                  // +1 = swipe left (next page), -1 = right
  void drag(bool down, int x, int y);   // mouse-driven finger
};

struct State {
  std::string root;  // the sim/ folder
  int encA = 0, encB = 0;
  bool buttonDown = false;
  uint32_t backlight = 255;
  std::mutex ledMutex;
  std::vector<uint32_t> leds;
  TouchSim touch;
  uint32_t lastWeb = 0;
};

extern State state;

void turnKnob(int detents);  // + = clockwise
void loadSounds();
void initAudio();
void startConsoleReader();
// Console lines starting with '@' are simulator commands: "@shot file.bmp", "@quit".
bool nextSimCommand(std::string &out);

}  // namespace sim
