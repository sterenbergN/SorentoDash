<#
.SYNOPSIS
  Installs all dependencies required to build, simulate, and flash SorentoDash.

.DESCRIPTION
  Sets up the project environment on a new machine:
  - Simulator: Downloads Zig (0.14.1), SDL2 (2.32.10), LovyanGFX, and ArduinoJson into sim\.tools
  - Firmware:  Installs arduino-cli, ESP32 board support (esp32:esp32), and required Arduino libraries
  - Python:    Installs Python dependencies (Pillow, pyserial) for tools and scripts

.PARAMETER SimOnly
  Only install dependencies needed for the desktop simulator.

.PARAMETER FirmwareOnly
  Only install dependencies needed for building and flashing ESP32 firmware.

.PARAMETER Force
  Force re-download and re-installation even if tools or libraries already exist.
#>
param(
  [switch]$SimOnly,
  [switch]$FirmwareOnly,
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
$proj = $PSScriptRoot
$sim = "$proj\sim"
$tools = "$sim\.tools"

$setupSim = -not $FirmwareOnly
$setupFirmware = -not $SimOnly

Write-Host "=== SorentoDash Environment Setup ===" -ForegroundColor Cyan

# -----------------------------------------------------------------------------
# 1. Simulator Toolchain & Libraries (Zig, SDL2, LovyanGFX, ArduinoJson)
# -----------------------------------------------------------------------------
if ($setupSim) {
  Write-Host "`n[1/3] Checking Simulator Toolchain..." -ForegroundColor Yellow
  New-Item -ItemType Directory -Force $tools | Out-Null

  # Zig compiler
  $zig = Get-ChildItem "$tools\zig-*\zig.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $zig -or $Force) {
    Write-Host "Downloading portable compiler (Zig 0.14.1) into sim\.tools..."
    $ProgressPreference = 'SilentlyContinue'
    $zigZip = "$tools\zig.zip"
    Invoke-WebRequest "https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip" -OutFile $zigZip
    Expand-Archive -Force $zigZip $tools
    Remove-Item $zigZip -Force
    $zig = Get-ChildItem "$tools\zig-*\zig.exe" | Select-Object -First 1
  }
  Write-Host "  [OK] Zig compiler: $($zig.FullName)" -ForegroundColor Green

  # SDL2
  $sdl = Get-ChildItem "$tools\SDL2-*" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $sdl -or $Force) {
    Write-Host "Downloading SDL2 (2.32.10) development libraries into sim\.tools..."
    $ProgressPreference = 'SilentlyContinue'
    $sdlZip = "$tools\sdl2.zip"
    Invoke-WebRequest "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip" -OutFile $sdlZip
    Expand-Archive -Force $sdlZip $tools
    Remove-Item $sdlZip -Force
    $sdl = Get-ChildItem "$tools\SDL2-*" -Directory | Select-Object -First 1
  }
  Write-Host "  [OK] SDL2: $($sdl.FullName)" -ForegroundColor Green

  # Simulator Arduino stand-in libraries (LovyanGFX & ArduinoJson)
  $toolsLibs = "$tools\libraries"
  $userLibs = "$env:USERPROFILE\OneDrive\Documents\Arduino\libraries"
  if (-not (Test-Path "$userLibs\LovyanGFX")) { $userLibs = "$env:USERPROFILE\Documents\Arduino\libraries" }

  $hasLgfx = (Test-Path "$userLibs\LovyanGFX\src") -or (Test-Path "$toolsLibs\LovyanGFX\src")
  $hasJson = (Test-Path "$userLibs\ArduinoJson\src") -or (Test-Path "$toolsLibs\ArduinoJson\src")

  if ((-not $hasLgfx -or -not $hasJson) -or $Force) {
    New-Item -ItemType Directory -Force $toolsLibs | Out-Null
    $ProgressPreference = 'SilentlyContinue'

    if (-not $hasLgfx -or $Force) {
      Write-Host "Downloading LovyanGFX for simulator into sim\.tools\libraries..."
      $lgZip = "$toolsLibs\lgfx.zip"
      Invoke-WebRequest "https://github.com/lovyan03/LovyanGFX/archive/refs/tags/1.2.30.zip" -OutFile $lgZip
      Expand-Archive -Force $lgZip $toolsLibs
      Remove-Item $lgZip -Force
      if (Test-Path "$toolsLibs\LovyanGFX") { Remove-Item -Recurse -Force "$toolsLibs\LovyanGFX" }
      $extractedLg = Get-ChildItem "$toolsLibs\LovyanGFX-*" -Directory | Select-Object -First 1
      if ($extractedLg) { Rename-Item $extractedLg.FullName "LovyanGFX" }
    }

    if (-not $hasJson -or $Force) {
      Write-Host "Downloading ArduinoJson for simulator into sim\.tools\libraries..."
      $jsonZip = "$toolsLibs\ajson.zip"
      Invoke-WebRequest "https://github.com/bblanchon/ArduinoJson/archive/refs/tags/v7.4.3.zip" -OutFile $jsonZip
      Expand-Archive -Force $jsonZip $toolsLibs
      Remove-Item $jsonZip -Force
      if (Test-Path "$toolsLibs\ArduinoJson") { Remove-Item -Recurse -Force "$toolsLibs\ArduinoJson" }
      $extractedJson = Get-ChildItem "$toolsLibs\ArduinoJson-*" -Directory | Select-Object -First 1
      if ($extractedJson) { Rename-Item $extractedJson.FullName "ArduinoJson" }
    }
  }
  Write-Host "  [OK] Simulator libraries ready" -ForegroundColor Green
}

# -----------------------------------------------------------------------------
# 2. Firmware Toolchain (arduino-cli, ESP32 core, Arduino libraries)
# -----------------------------------------------------------------------------
if ($setupFirmware) {
  Write-Host "`n[2/3] Checking Firmware Toolchain (arduino-cli & ESP32-S3)..." -ForegroundColor Yellow

  # Locate or install arduino-cli
  $cli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source
  if (-not $cli -or -not (Test-Path $cli)) {
    $cli = "$env:LOCALAPPDATA\arduino-cli\arduino-cli.exe"
  }
  if (-not (Test-Path $cli)) {
    $claudeCli = Get-ChildItem "$env:LOCALAPPDATA\Packages\Claude_*\LocalCache\Local\arduino-cli\arduino-cli.exe" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName -First 1
    if ($claudeCli -and (Test-Path $claudeCli)) { $cli = $claudeCli }
  }

  if ((-not (Test-Path $cli)) -or $Force) {
    Write-Host "Installing arduino-cli into $env:LOCALAPPDATA\arduino-cli..."
    $cliDir = "$env:LOCALAPPDATA\arduino-cli"
    New-Item -ItemType Directory -Force $cliDir | Out-Null
    $cliZip = "$cliDir\arduino-cli.zip"
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest "https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip" -OutFile $cliZip
    Expand-Archive -Force $cliZip $cliDir
    Remove-Item $cliZip -Force
    $cli = "$cliDir\arduino-cli.exe"
  }
  Write-Host "  [OK] arduino-cli: $cli" -ForegroundColor Green

  # Configure ESP32 package index
  $espUrl = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
  & $cli config init --additional-urls $espUrl -ErrorAction SilentlyContinue 2>$null | Out-Null
  & $cli config set board_manager.additional_urls "[$espUrl]" 2>$null | Out-Null

  Write-Host "Updating index..."
  & $cli core update-index --additional-urls $espUrl | Out-Null

  # Check / install ESP32 platform
  $cores = & $cli core list
  if ($cores -notmatch 'esp32:esp32' -or $Force) {
    Write-Host "Installing ESP32 core (esp32:esp32) for CrowPanel ESP32-S3..."
    & $cli core install esp32:esp32 --additional-urls $espUrl
  } else {
    Write-Host "  [OK] ESP32 core is installed" -ForegroundColor Green
  }

  # Install required Arduino libraries
  $arduinoLibs = @("LovyanGFX", "ArduinoJson", "Adafruit NeoPixel", "NimBLE-Arduino")
  foreach ($lib in $arduinoLibs) {
    Write-Host "Checking Arduino library: $lib..."
    & $cli lib install $lib | Out-Null
  }
  Write-Host "  [OK] Arduino libraries installed" -ForegroundColor Green
}

# -----------------------------------------------------------------------------
# 3. Python Dependencies (for tools/console.py, make_fonts.py, sim_drive.py)
# -----------------------------------------------------------------------------
Write-Host "`n[3/3] Checking Python Dependencies..." -ForegroundColor Yellow
$py = Get-Command python -ErrorAction SilentlyContinue
if ($py) {
  $req = "$proj\requirements.txt"
  if (Test-Path $req) {
    Write-Host "Installing Python requirements via pip..."
    & python -m pip install -r $req --quiet
    Write-Host "  [OK] Python dependencies installed" -ForegroundColor Green
  }
} else {
  Write-Host "  [-] Python not found in PATH. (Optional: install Python 3 to use tools\console.py or make_fonts.py)" -ForegroundColor Gray
}

Write-Host "`n=== Setup Complete! ===" -ForegroundColor Green
Write-Host "  - To launch simulator: .\sim\run.ps1"
Write-Host "  - To build/flash ESP32: .\flash.ps1 -Port COM3"
