// Raw capture of the TM7 receiver's DOUT signal (433.92 MHz tire sensors),
// used to work out the sensor protocol before writing a decoder.
//
// Edges are timestamped in an interrupt. A burst of edges followed by a quiet
// gap is printed to the USB console as pulse widths in microseconds:
// positive = time spent high, negative = time spent low.
#pragma once
#include <Arduino.h>
#include <driver/gpio.h>

class RfCapture {
public:
  static constexpr uint32_t GAP_US = 4000;  // quiet time that ends a burst
  static constexpr int MIN_EDGES = 40;      // shorter bursts are noise
  static constexpr int MAX_EDGES = 1500;

  bool enabled = false;

  void begin(int p) {
    pin = p;
    pinMode(pin, INPUT);
    attachInterruptArg(digitalPinToInterrupt(pin), isr, this, CHANGE);
  }

  // Edges seen in the last completed second (a rough noise indicator).
  uint32_t edgesPerSecond() const { return rate; }

  void poll(Stream &out) {
    uint32_t nowMs = millis();
    if (nowMs - rateAt >= 1000) {
      rate = edgeCount;
      edgeCount = 0;
      rateAt = nowMs;
    }
    if (!enabled || head == 0 || micros() - lastEdge < GAP_US) return;

    portENTER_CRITICAL(&mux);
    int n = head;
    bool full = overflow;
    memcpy(copy, (const void *)buf, n * sizeof(int32_t));
    head = 0;
    overflow = false;
    portEXIT_CRITICAL(&mux);
    if (n < MIN_EDGES) return;

    Serial.setTxTimeoutMs(300);  // make sure the whole burst gets out
    out.printf("RF %d edges%s @%lu ms:", n, full ? " (truncated)" : "", (unsigned long)nowMs);
    for (int i = 0; i < n; i++) {
      if (i % 24 == 0) out.print("\n ");
      out.printf(" %ld", (long)copy[i]);
    }
    out.println();
    Serial.setTxTimeoutMs(0);
  }

private:
  int pin = -1;
  volatile int head = 0;
  volatile bool overflow = false;
  volatile uint32_t lastEdge = 0, edgeCount = 0;
  uint32_t rate = 0, rateAt = 0;
  volatile int32_t buf[MAX_EDGES];
  int32_t copy[MAX_EDGES];
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

  static void isr(void *arg);
};

// Defined outside the class: an IRAM function defined inline in a class body
// trips a linker literal-placement error on ESP32.
void IRAM_ATTR RfCapture::isr(void *arg) {
  RfCapture *c = (RfCapture *)arg;
  uint32_t t = micros();
  int level = gpio_get_level((gpio_num_t)c->pin);
  uint32_t d = t - c->lastEdge;
  c->lastEdge = t;
  c->edgeCount++;
  if (!c->enabled) return;
  portENTER_CRITICAL_ISR(&c->mux);
  if (d > GAP_US) {
    c->head = 0;  // a new burst starts; the gap itself isn't a pulse
  } else if (c->head < MAX_EDGES) {
    // `d` is how long the line sat at the level before this edge.
    c->buf[c->head++] = level ? -(int32_t)d : (int32_t)d;
  } else {
    c->overflow = true;
  }
  portEXIT_CRITICAL_ISR(&c->mux);
}
