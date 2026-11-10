// The simulated board: what the Arduino/ESP32 stand-ins talk to instead of
// real hardware. Implemented in SimHardware.cpp, driven by the SDL window.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace sim {

// GPIO: knob (encoder A/B + push button) and anything else digitalRead sees.
int readPin(int pin);
void writePin(int pin, int level);
using Isr = void (*)(void *);
void attachIsr(int pin, Isr fn, void *arg);

// Backlight (ledcWrite on the backlight pin), 0..255.
void setPwm(int pin, uint32_t value);

// LED ring (Adafruit_NeoPixel::show).
void setLeds(const uint32_t *rgb, int count);

// CST816D touch controller registers, read over the simulated I2C bus.
int touchRead(uint8_t reg, uint8_t *buf, int n);

// Audio FX sound board on Serial1.
void soundboardWrite(uint8_t c);
int soundboardAvailable();
int soundboardRead();
void soundboardOpen(bool open);

// USB console (Serial): output to stdout, input from stdin lines.
void consoleWrite(const uint8_t *buf, size_t n);
int consoleAvailable();
int consoleRead();

// Where saved state lives (prefs, FAT files).
std::string stateDir();

// Web server activity (for the Wi-Fi screen's "phone connected" state).
void noteWebRequest();
bool recentWebRequest();

}  // namespace sim
