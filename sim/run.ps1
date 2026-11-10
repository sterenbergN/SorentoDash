# Build (if anything changed) and run the Sorento Dash simulator.
#   .\sim\run.ps1          build + run
#   .\sim\run.ps1 -Fresh   start from factory settings (wipes sim\state)
param([switch]$Fresh)
$sim = $PSScriptRoot
if ($Fresh -and (Test-Path "$sim\state")) { Remove-Item -Recurse -Force "$sim\state" }

& "$sim\build.ps1"
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# The firmware's "Restart" (Settings > System) exits with code 3: start it again.
do {
  & "$sim\build\SorentoSim.exe"
  $code = $LASTEXITCODE
} while ($code -eq 3)
