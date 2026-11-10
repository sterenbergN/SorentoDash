# Update the display over Wi-Fi: join the "SorentoDash" network first
# (Menu > Phone setup on the display), then run this.
param([string]$Bin = "$PSScriptRoot\build\SorentoDash.ino.bin", [string]$Url = "http://192.168.4.1/update")
curl.exe --fail-with-body -F "firmware=@$Bin" $Url
