# Build SorentoDash and upload it to the CrowPanel over USB.
#   .\flash.ps1              build + upload on COM3
#   .\flash.ps1 -BuildOnly   just build (for a Wi-Fi update: build\SorentoDash.ino.bin)
param([string]$Port = "COM3", [switch]$BuildOnly)
$cli = "$env:LOCALAPPDATA\arduino-cli\arduino-cli.exe"
$fqbn = "esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,FlashMode=qio"
& $cli compile -b $fqbn --output-dir "$PSScriptRoot\build" $PSScriptRoot
if ($LASTEXITCODE -ne 0) { exit 1 }
if ($BuildOnly) { Write-Host "Firmware: $PSScriptRoot\build\SorentoDash.ino.bin"; exit 0 }
& $cli upload -p $Port -b $fqbn --input-dir "$PSScriptRoot\build" $PSScriptRoot
