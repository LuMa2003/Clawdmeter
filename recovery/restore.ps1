# restore.ps1 — Flash the most recent factory snapshot in this folder.
#
# Usage:
#   .\recovery\restore.ps1                     # auto-detect COM port
#   .\recovery\restore.ps1 -Port COM3          # explicit port
#   .\recovery\restore.ps1 -Image foo.bin      # explicit image
#
# Why this script exists: a known-good factory.bin lives next to it so a
# bricked or misbehaving build can be reverted without rebuilding from source.

param(
    [string]$Port,
    [string]$Image
)

$ErrorActionPreference = "Stop"
$RecoveryDir = $PSScriptRoot

# Pick the newest factory.bin if not given.
if (-not $Image) {
    $candidates = Get-ChildItem $RecoveryDir -Filter "*.factory.bin" | Sort-Object LastWriteTime -Descending
    if ($candidates.Count -eq 0) {
        throw "No *.factory.bin found in $RecoveryDir"
    }
    $Image = $candidates[0].FullName
} elseif (-not (Test-Path $Image)) {
    $Image = Join-Path $RecoveryDir $Image
    if (-not (Test-Path $Image)) { throw "Image not found: $Image" }
}

# Locate esptool. Prefer PlatformIO's own penv (its bundled python has esptool
# installed); fall back to `python -m esptool` if the user pip-installed it
# globally. The packaged esptool.py wrapper alone doesn't work — it needs the
# venv where its deps live.
$PioPython = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
if (Test-Path $PioPython) {
    $EspPython = $PioPython
} else {
    $EspPython = "python"
}

# Auto-detect the ESP32-S3 USB-Serial/JTAG port if not given.
# VID 0x303A = Espressif Systems, PID 0x1001 = ESP32-S3 native USB.
if (-not $Port) {
    $pnp = Get-PnpDevice -Class Ports -ErrorAction SilentlyContinue | Where-Object {
        $_.Status -eq "OK" -and $_.InstanceId -like "*VID_303A*"
    }
    if ($pnp) {
        if ($pnp.FriendlyName -match "\((COM\d+)\)") { $Port = $Matches[1] }
    }
    if (-not $Port) { throw "Could not auto-detect ESP32-S3 COM port. Pass -Port COM<n> manually." }
}

Write-Host "=== Clawdmeter recovery flash ==="
Write-Host "Image: $Image"
Write-Host "Port:  $Port"
Write-Host ""

$env:PYTHONIOENCODING = "utf-8"
chcp 65001 | Out-Null

& $EspPython -m esptool --chip esp32s3 --port $Port --baud 921600 `
    write_flash 0x0 $Image

if ($LASTEXITCODE -ne 0) { throw "esptool failed (exit $LASTEXITCODE)" }

Write-Host ""
Write-Host "=== Restore complete. Device should reboot automatically. ==="
