# Build SorentoDash and upload it to the CrowPanel over USB.
#   .\flash.ps1              build + upload on COM3
#   .\flash.ps1 -BuildOnly   just build (for a Wi-Fi update: build\SorentoDash.ino.bin)
param([string]$Port = "COM3", [switch]$BuildOnly)
# Locate arduino-cli
$cli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source
if (-not $cli -or -not (Test-Path $cli)) {
  $cli = "$env:LOCALAPPDATA\arduino-cli\arduino-cli.exe"
}
if (-not (Test-Path $cli)) {
  $claudeCli = Get-ChildItem "$env:LOCALAPPDATA\Packages\Claude_*\LocalCache\Local\arduino-cli\arduino-cli.exe" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName -First 1
  if ($claudeCli -and (Test-Path $claudeCli)) { $cli = $claudeCli }
}
if (-not (Test-Path $cli)) {
  Write-Host "arduino-cli not found. Running setup.ps1 -FirmwareOnly..."
  & "$PSScriptRoot\setup.ps1" -FirmwareOnly
  $cli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source
  if (-not $cli) { $cli = "$env:LOCALAPPDATA\arduino-cli\arduino-cli.exe" }
}
if (-not (Test-Path $cli)) {
  Write-Error "arduino-cli could not be found or installed. Please run .\setup.ps1 first."
  exit 1
}
$fqbn = "esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,FlashMode=qio"
& $cli compile -b $fqbn --output-dir "$PSScriptRoot\build" $PSScriptRoot
if ($LASTEXITCODE -ne 0) { exit 1 }
if ($BuildOnly) { Write-Host "Firmware: $PSScriptRoot\build\SorentoDash.ino.bin"; exit 0 }
& $cli upload -p $Port -b $fqbn --input-dir "$PSScriptRoot\build" $PSScriptRoot
