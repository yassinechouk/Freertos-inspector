<#
  Build the ESP32 #1 firmware in Docker, then flash it from Windows.

  Why: Smart App Control blocks the unsigned ESP-IDF toolchain on Windows, so the
  build runs in the official espressif/idf container (build-docker/). Flashing
  only needs esptool (Python), which is not blocked.

  Usage (from esp32-motor-sim/):
    .\build_flash.ps1                 build + flash on COM11
    .\build_flash.ps1 -Port COM7      other serial port
    .\build_flash.ps1 -NoFlash        build only
  Close any serial monitor first: it keeps the port open and the flash fails.
#>
param(
  [string]$Port = 'COM11',
  [switch]$NoFlash
)

$ErrorActionPreference = 'Stop'
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$python = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
$env:PATH = 'C:\Program Files\Docker\Docker\resources\bin;' + $env:PATH

# Docker Desktop must be running (the engine answers).
docker version --format '{{.Server.Version}}' *> $null
if ($LASTEXITCODE -ne 0) { throw 'Docker engine not running: start Docker Desktop and wait for "Engine running".' }

Set-Location $here
docker run --rm -v "${here}:/project" -w /project espressif/idf:v6.1 idf.py -B build-docker build
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

if ($NoFlash) { Write-Host 'Build done (flash skipped).'; return }

Set-Location (Join-Path $here 'build-docker')
& $python -m esptool --chip esp32s3 -p $Port -b 460800 write-flash '@flash_args'
if ($LASTEXITCODE -ne 0) { throw "Flash failed on $Port (port busy? close the serial monitor)." }
Write-Host "Flashed on $Port. Open the serial monitor to check the 'moteur :' line."
