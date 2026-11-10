# Print the board's USB console for N seconds without resetting it.
param([string]$Port = "COM3", [int]$Seconds = 10)
$p = New-Object System.IO.Ports.SerialPort $Port, 115200
$p.DtrEnable = $false
$p.RtsEnable = $false
$p.Open()
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) { $s = $p.ReadExisting(); if ($s) { Write-Host -NoNewline $s }; Start-Sleep -Milliseconds 100 }
$p.Close()
