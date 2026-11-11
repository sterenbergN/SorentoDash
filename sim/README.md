# Sorento Dash simulator

Runs the **real firmware** (`SorentoDash.ino` and its headers, unchanged) on
the PC, so UI changes can be tried before building an update for the car.

```
.\sim\run.ps1           # build if needed, then run
.\sim\run.ps1 -Fresh    # start from factory settings
```

The first build downloads a portable compiler (Zig) and SDL2 into
`sim\.tools` and compiles LovyanGFX once; after that a rebuild takes a few
seconds.

## Controls

| Knob | |
|---|---|
| Turn | mouse wheel, or Left / Right arrow |
| Press | left-click the screen, or Space / Enter (hold = long press) |
| Double-click | two quick presses |
| Swipe | right-drag across the screen, or `[` / `]` |
| Quit | Esc |

The terminal is the board's USB console: type `status`, `json`, `cw 3`,
`press`, `long`, `double`, `swipe l` and so on. Lines starting with `@` go to
the simulator: `@shot file.bmp` saves the window, `@turn N`, `@down`, `@up`,
`@swipe l|r` drive the simulated hardware, `@quit` exits.
`tools\sim_drive.py` scripts these for automated screenshots.

## What's simulated

- **Display, knob, touch, backlight, LED ring**: the window. The knob feeds real
  quadrature edges into the firmware's encoder interrupt.
- **Audio FX sound board**: speaks the same serial protocol as the real board.
  Put `.wav` files in `sim\sounds` to hear your own horns (names are cut to
  8.3 like on the board); without any, six built-in tones are used.
- **OBD-II**: Bluetooth never finds an adapter, so the demo car is switched on
  for a fresh state (Settings > Car > OBD adapter > Demo car).
- **Phone pages**: opening Settings > Phone & Wi-Fi > Phone setup starts the
  web server at **http://localhost:8080** (setup page and `/stats`).
- **Saved data**: settings, service items and drive history live in
  `sim\state` (delete it, or use `-Fresh`, to start over).

Not simulated: firmware updates over Wi-Fi, a real OBD adapter, OGG playback.

## How it works

`build.ps1` compiles `SorentoDash.ino` directly as C++ with `-DSORENTO_SIM`
(no Arduino tools needed: the sketch defines every function before using it,
so keep it that way) against the stand-ins in `sim\shim` (Arduino core,
FreeRTOS, Preferences, LittleFS, WebServer, Wire, NeoPixel, NimBLE), and links it
with `SimHardware.cpp` and `main.cpp`. The only firmware change for this is the
`SORENTO_SIM` screen in `Board.h`.
