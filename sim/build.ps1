# Build the Sorento Dash desktop simulator (sim\build\SorentoSim.exe).
# The firmware itself is compiled unchanged, against desktop stand-ins for the
# Arduino/ESP32 libraries in sim\shim. Needs no Arduino tools - only the
# LovyanGFX and ArduinoJson libraries in Documents\Arduino\libraries. First run downloads a portable compiler
# (Zig) and SDL2 into sim\.tools, and compiles LovyanGFX once (cached).
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$sim = $PSScriptRoot
$proj = Split-Path $sim
$tools = "$sim\.tools"
$out = "$sim\build"
$libs = "$env:USERPROFILE\OneDrive\Documents\Arduino\libraries"
if (-not (Test-Path $libs)) { $libs = "$env:USERPROFILE\Documents\Arduino\libraries" }

# ---- toolchain
$zig = Get-ChildItem "$tools\zig-*\zig.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
$sdl = Get-ChildItem "$tools\SDL2-*" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $zig -or -not $sdl) {
  Write-Host "Downloading compiler (Zig) and SDL2 into sim\.tools ..."
  New-Item -ItemType Directory -Force $tools | Out-Null
  $ProgressPreference = 'SilentlyContinue'
  Invoke-WebRequest "https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip" -OutFile "$tools\zig.zip"
  Invoke-WebRequest "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip" -OutFile "$tools\sdl2.zip"
  Expand-Archive -Force "$tools\zig.zip" $tools; Expand-Archive -Force "$tools\sdl2.zip" $tools
  Remove-Item "$tools\zig.zip", "$tools\sdl2.zip"
  $zig = Get-ChildItem "$tools\zig-*\zig.exe" | Select-Object -First 1
  $sdl = Get-ChildItem "$tools\SDL2-*" -Directory | Select-Object -First 1
}
$sdlRoot = "$($sdl.FullName)\x86_64-w64-mingw32"
if ($Clean -and (Test-Path $out)) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force "$out\obj\lgfx" | Out-Null

$common = @('-target', 'x86_64-windows-gnu', '-O1', '-g0', '-w',
  '-DLGFX_SDL', '-DSORENTO_SIM', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1',
  "-I$sim\shim", "-I$proj", "-I$libs\LovyanGFX\src", "-I$libs\ArduinoJson\src", "-I$sdlRoot\include", "-I$sdlRoot\include\SDL2")

function Compile($src, $obj, $lang) {
  $std = if ($lang -eq 'c') { '-std=gnu11' } else { '-std=gnu++17' }
  $drv = if ($lang -eq 'c') { 'cc' } else { 'c++' }
  & $zig.FullName $drv @common $std -c $src -o $obj
  if ($LASTEXITCODE -ne 0) { throw "compile failed: $src" }
}

# ---- LovyanGFX (once)
$lg = "$libs\LovyanGFX\src\lgfx"
$lgSources = @()
$lgSources += Get-ChildItem "$lg\v1\*.cpp", "$lg\v1\misc\*.cpp", "$lg\v1\platforms\sdl\*.cpp" -ErrorAction SilentlyContinue
$lgSources += Get-ChildItem "$lg\utility\*.c", "$lg\Fonts\efont\*.c", "$lg\Fonts\IPA\*.c", "$lg\Fonts\lvgl\*.c", "$lg\v1\lv_font\*.c" -ErrorAction SilentlyContinue
$lgObjs = @()
$built = 0
foreach ($f in $lgSources) {
  $rel = $f.FullName.Substring($lg.Length + 1) -replace '[\\/.]', '_'
  $obj = "$out\obj\lgfx\$rel.o"
  $lgObjs += $obj
  if (-not (Test-Path $obj)) {
    if ($built -eq 0) { Write-Host "Compiling LovyanGFX for the desktop (first build only) ..." }
    Compile $f.FullName $obj ($(if ($f.Extension -eq '.c') { 'c' } else { 'cpp' }))
    $built++
  }
}

# ---- the sketch, compiled straight from the .ino as C++ (it defines every
# function before using it, so Arduino's generated prototypes aren't needed).
Write-Host "Compiling firmware + simulator ..."
& $zig.FullName c++ @common -std=gnu++17 -include Arduino.h -x c++ -c "$proj\SorentoDash.ino" -o "$out\obj\sketch.o"
if ($LASTEXITCODE -ne 0) { throw "compile failed: SorentoDash.ino" }
Compile "$sim\SimHardware.cpp" "$out\obj\hardware.o" 'cpp'
Compile "$sim\main.cpp" "$out\obj\main.o" 'cpp'

& $zig.FullName c++ -target x86_64-windows-gnu -o "$out\SorentoSim.exe" "$out\obj\sketch.o" "$out\obj\hardware.o" "$out\obj\main.o" @lgObjs "$sdlRoot\lib\libSDL2.dll.a" -lws2_32 -lgdi32 -luser32
if ($LASTEXITCODE -ne 0) { throw "link failed" }
Copy-Item "$sdlRoot\bin\SDL2.dll" $out -Force
Write-Host "Built $out\SorentoSim.exe"
