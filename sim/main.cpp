// Sorento Dash desktop simulator: runs the real firmware (setup()/loop())
// against simulated hardware and shows the round display in a window.
//
//   Turn the knob   mouse wheel, or Left/Right arrow
//   Press the knob  left click on the display, or Space/Enter (hold = long press)
//   Swipe           right-drag on the display, or [ and ] keys
//   Quit            Esc
//
// The terminal is the board's USB console (status, cw, press, long, swipe l...)
// plus simulator commands: "@shot file.bmp" saves the window, "@quit" exits,
// "@turn N", "@down", "@up", "@swipe l|r" act on the simulated hardware.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <filesystem>
#include "shim/Arduino.h"
#include "SimState.h"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// From the sketch: the display it draws on.
#include "../Board.h"
extern LGFX lcd;

namespace fs = std::filesystem;

constexpr int W = 600, H = 700;
constexpr int CX = W / 2, CY = 300, SCALE = 2, R = 120 * SCALE;

static lgfx::LGFX_Sprite frame;

static void drawFrame() {
  frame.fillScreen(0x101114U);
  // Knob ring around the screen.
  frame.fillCircle(CX, CY, R + 46, 0x2B2D31U);
  frame.fillCircle(CX, CY, R + 30, 0x1B1C1FU);
  // The screen, 2x, then a bezel ring that hides the square corners.
  // Written straight into the frame buffer (R,G,B bytes): ~230k pixels a frame.
  const uint16_t *src = (const uint16_t *)lcd.getBuffer();
  uint8_t *fb = (uint8_t *)frame.getBuffer();
  int b = std::max(13, (int)sim::state.backlight);  // backlight dims the screen
  for (int y = 0; y < 2 * R; y++) {
    int dy = y - R;
    for (int x = 0; x < 2 * R; x++) {
      int dx = x - R;
      if (dx * dx + dy * dy > R * R) continue;
      uint16_t v = src[(y / SCALE) * 240 + (x / SCALE)];
      v = (uint16_t)((v >> 8) | (v << 8));  // sprite buffers are big-endian RGB565
      uint8_t *p = fb + ((size_t)(CY - R + y) * W + (CX - R + x)) * 3;
      p[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31 * b / 255);
      p[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63 * b / 255);
      p[2] = (uint8_t)((v & 31) * 255 / 31 * b / 255);
    }
  }
  frame.drawCircle(CX, CY, R, 0x2B2D31U);
  // LED ring: five LEDs spread around the knob.
  {
    std::lock_guard<std::mutex> l(sim::state.ledMutex);
    int n = (int)sim::state.leds.size();
    for (int i = 0; i < n; i++) {
      float a = (90 + (i - (n - 1) / 2.0f) * 36) * DEG_TO_RAD;  // fanned around the bottom
      int x = CX + cosf(a) * (R + 38), y = CY + sinf(a) * (R + 38);
      // Show the LED's hue at full saturation, faded by how bright it's driven.
      uint32_t c = sim::state.leds[i];
      int r = (c >> 16) & 255, g = (c >> 8) & 255, bl = c & 255, m = std::max(r, std::max(g, bl));
      float k = m ? std::sqrt(m / 255.0f) : 0;
      auto mix = [&](int on, int off) { return (int)(off + (on * 255 / std::max(m, 1) - off) * k); };
      uint32_t col = m ? (uint32_t)(mix(r, 0x3A) << 16 | mix(g, 0x3C) << 8 | mix(bl, 0x42)) : 0x3A3C42U;
      frame.fillCircle(x, y, 8, col);
    }
  }
  // Legend.
  frame.setTextDatum(lgfx::textdatum_t::top_center);
  frame.setFont(&lgfx::fonts::FreeSans9pt7b);
  frame.setTextColor(0x9CA3AFU);
  frame.drawString("Turn: mouse wheel or Left/Right    Press: click or Space (hold = long)", CX, H - 108);
  frame.drawString("Swipe: right-drag or [ ]    Double-click: two quick presses", CX, H - 84);
  frame.drawString("Console commands in the terminal (e.g. status)    Quit: Esc", CX, H - 60);
  frame.setTextColor(0x6B7280U);
  frame.drawString("Sorento Dash simulator - runs the real firmware", CX, H - 30);
}

// Map window coordinates to the 240x240 display.
static bool toDisplay(int wx, int wy, int &x, int &y) {
  int dx = wx - CX, dy = wy - CY;
  x = (dx + R) / SCALE;
  y = (dy + R) / SCALE;
  return dx * dx + dy * dy <= R * R;
}

int main(int, char **) {
  // sim/ folder = parent of the folder holding this exe (sim/build/).
  char *base = SDL_GetBasePath();
  sim::state.root = fs::path(base ? base : ".").parent_path().parent_path().string();
  SDL_free(base);

  // First run: start with the demo car on, so the car pages have data.
  fs::path obdPrefs = fs::path(sim::state.root) / "state" / "prefs" / "obd.txt";
  if (!fs::exists(obdPrefs)) {
    fs::create_directories(obdPrefs.parent_path());
    FILE *f = fopen(obdPrefs.string().c_str(), "w");
    if (f) { fputs("sim\t1\n", f); fclose(f); }
  }

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
    printf("SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *win = SDL_CreateWindow("Sorento Dash simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, 0);
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, W, H);
  frame.setColorDepth(24);
  frame.createSprite(W, H);

  sim::initAudio();
  sim::loadSounds();
  sim::startConsoleReader();
  printf("[sim] state folder: %s\n", (fs::path(sim::state.root) / "state").string().c_str());

  setup();

  bool running = true, leftDown = false, rightDown = false;
  uint32_t lastPresent = 0;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT: running = false; break;
        case SDL_MOUSEWHEEL: sim::turnKnob(e.wheel.y < 0 ? 1 : -1); break;
        case SDL_KEYDOWN:
          if (e.key.repeat) break;
          switch (e.key.keysym.sym) {
            case SDLK_ESCAPE: running = false; break;
            case SDLK_RIGHT: case SDLK_DOWN: case SDLK_d: sim::turnKnob(1); break;
            case SDLK_LEFT: case SDLK_UP: case SDLK_a: sim::turnKnob(-1); break;
            case SDLK_SPACE: case SDLK_RETURN: sim::state.buttonDown = true; break;
            case SDLK_RIGHTBRACKET: case SDLK_e: sim::state.touch.swipe(1); break;
            case SDLK_LEFTBRACKET: case SDLK_q: sim::state.touch.swipe(-1); break;
          }
          break;
        case SDL_KEYUP:
          if (e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_RETURN) sim::state.buttonDown = false;
          break;
        case SDL_MOUSEBUTTONDOWN: {
          int x, y;
          bool on = toDisplay(e.button.x, e.button.y, x, y);
          if (e.button.button == SDL_BUTTON_LEFT && on) { leftDown = true; sim::state.buttonDown = true; }
          if (e.button.button == SDL_BUTTON_RIGHT && on) { rightDown = true; sim::state.touch.drag(true, x, y); }
          break;
        }
        case SDL_MOUSEMOTION:
          if (rightDown) {
            int x, y;
            toDisplay(e.motion.x, e.motion.y, x, y);
            sim::state.touch.drag(true, x, y);
          }
          break;
        case SDL_MOUSEBUTTONUP:
          if (e.button.button == SDL_BUTTON_LEFT && leftDown) { leftDown = false; sim::state.buttonDown = false; }
          if (e.button.button == SDL_BUTTON_RIGHT && rightDown) {
            rightDown = false;
            int x, y;
            toDisplay(e.button.x, e.button.y, x, y);
            sim::state.touch.drag(false, x, y);
          }
          break;
      }
    }

    std::string cmd;
    while (sim::nextSimCommand(cmd)) {
      if (cmd == "quit") running = false;
      // Same paths as the mouse/keyboard: quadrature edges, button pin, touch.
      if (cmd.rfind("turn ", 0) == 0) sim::turnKnob(atoi(cmd.c_str() + 5));
      if (cmd == "down") sim::state.buttonDown = true;
      if (cmd == "up") sim::state.buttonDown = false;
      if (cmd == "swipe l") sim::state.touch.swipe(1);
      if (cmd == "swipe r") sim::state.touch.swipe(-1);
      if (cmd.rfind("shot ", 0) == 0) {  // save the window as a BMP
        drawFrame();
        SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(frame.getBuffer(), W, H, 24, W * 3, SDL_PIXELFORMAT_RGB24);
        SDL_SaveBMP(surf, cmd.substr(5).c_str());
        SDL_FreeSurface(surf);
        printf("[sim] saved %s%c", cmd.substr(5).c_str(), 10);
      }
    }

    loop();

    uint32_t now = SDL_GetTicks();
    if (now - lastPresent >= 16) {
      lastPresent = now;
      drawFrame();
      SDL_UpdateTexture(tex, nullptr, frame.getBuffer(), W * 3);
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, tex, nullptr, nullptr);
      SDL_RenderPresent(ren);
    } else {
      SDL_Delay(1);
    }
  }
  SDL_Quit();
  return 0;
}
