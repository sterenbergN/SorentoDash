# SorentoDash

Custom rotary horn selector, vehicle metrics display, and trip computer for Kia Sorento, built for the **CrowPanel 1.28" ESP32-S3 rotary knob display**.

- **Car Dashboard**: Live OBD-II metrics (coolant temp, transmission temp, fuel trims, battery voltage, speed, RPM) via Bluetooth LE ELM327 adapter.
- **Rotary Horn Selector**: Select and trigger horns/sound effects via an Adafruit Audio FX board over UART.
- **Web Setup & OTA**: Built-in Wi-Fi portal for configuring horns, car settings, and wireless firmware updates.
- **PC Simulator**: Full desktop simulator using SDL2 and portable Zig compiler, running firmware C++ unchanged.

---

## Setup on a New Device

Clone the repository and run the automated setup script in PowerShell:

```powershell
# Complete setup: simulator compiler, SDL2, Arduino CLI, ESP32 core, and libraries
.\setup.ps1
```

### Setup Options
- `.\setup.ps1` — Complete setup for both simulator and firmware flashing.
- `.\setup.ps1 -SimOnly` — Fast setup for PC simulator only (no Arduino installation required).
- `.\setup.ps1 -FirmwareOnly` — Sets up Arduino CLI, ESP32 core, and Arduino libraries for hardware builds.
- `.\setup.ps1 -Force` — Force re-download / reinstall tools.

---

## Running the Desktop Simulator

You can test UI changes and dashboard features directly on your PC:

```powershell
.\sim\run.ps1          # Build (if changed) and launch simulator
.\sim\run.ps1 -Fresh   # Start from factory defaults (clears local sim\state)
```

- **Turn Knob**: Mouse wheel, or Left / Right arrow keys
- **Press**: Left-click or Space / Enter (hold for long press)
- **Double-click**: Two quick presses (opens horn menu)
- **Swipe**: Right-drag across screen, or `[` / `]`
- **Quit**: Esc

See [`sim/README.md`](sim/README.md) for more details.

---

## Flashing the Hardware

To compile and upload firmware to the CrowPanel display over USB:

```powershell
.\flash.ps1                  # Build and flash on default COM3
.\flash.ps1 -Port COM5       # Build and flash on a specific COM port
.\flash.ps1 -BuildOnly       # Build firmware binary without uploading
```

### Over-The-Air (OTA) Wi-Fi Updates

1. On the display, open **Settings > Phone & Wi-Fi > Phone setup** to start Wi-Fi.
2. Connect your PC to the `SorentoDash` Wi-Fi network.
3. Run:
```powershell
.\ota.ps1
```
