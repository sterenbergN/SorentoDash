// Pin map and display driver config for the ELECROW CrowPanel 1.28" ESP32-S3
// Rotary Display (GC9A01 240x240 round IPS, rotary encoder, 5x WS2812 ring).
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// Display power rails must be driven HIGH before the panel is initialised.
constexpr int PIN_LCD_PWR1   = 1;
constexpr int PIN_LCD_PWR2   = 2;
constexpr int PIN_BACKLIGHT  = 46;
constexpr int PIN_POWER_LED  = 40;

constexpr int PIN_ENC_A      = 45;
constexpr int PIN_ENC_B      = 42;
constexpr int PIN_ENC_SW     = 41;

constexpr int PIN_LED_RING   = 48;
constexpr int LED_RING_COUNT = 5;

// 4-pin "UART" JST 1.25mm connector (UART0 pins, free because USB CDC is used
// for the console). Wired to the Adafruit Audio FX board.
constexpr int PIN_FX_TX      = 43;  // -> Audio FX "RX"
constexpr int PIN_FX_RX      = 44;  // <- Audio FX "TX"
// Connector pin order (both UART and I2C): 1=RX/SCL 2=TX/SDA 3=5V 4=GND.
// Signals pass through 5V MOSFET level shifters on the UART connector.

// CST816D capacitive touch controller.
constexpr int PIN_TP_SDA     = 6;
constexpr int PIN_TP_SCL     = 7;
constexpr int PIN_TP_RST     = 13;

// 4-pin "I2C" connector.
constexpr int PIN_I2C_SDA    = 38;
constexpr int PIN_I2C_SCL    = 39;

// TM7 TPMS receiver board "DOUT" test point -> I2C connector pin 1.
constexpr int PIN_RF_DOUT    = PIN_I2C_SCL;

#ifdef SORENTO_SIM
// Desktop simulator (sim/): the "panel" is a 240x240 memory buffer that the
// simulator window shows.
class LGFX : public lgfx::LGFX_Sprite {
public:
  LGFX() { setColorDepth(16); }
  bool init() { return createSprite(240, 240) != nullptr; }
};
#else
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI _bus;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 80000000;
      cfg.freq_read = 20000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 10;
      cfg.pin_mosi = 11;
      cfg.pin_miso = -1;
      cfg.pin_dc = 3;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 9;
      cfg.pin_rst = 14;
      cfg.pin_busy = -1;
      cfg.memory_width = 240;
      cfg.memory_height = 240;
      cfg.panel_width = 240;
      cfg.panel_height = 240;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};
#endif  // SORENTO_SIM
