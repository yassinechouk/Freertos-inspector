<#
  Dashboard tests, in Docker (no local Node needed):
    1. syntax of every module + unit tests of the schedulability analysis (node);
    2. UI smoke test in headless Chromium (Playwright) with the synthetic fixture session:
       every view, light and dark themes, phone width; fails on any JavaScript error.
  Screenshots are written to build/ui-screenshots/ (ignored by git).
  Behind a TLS-inspecting antivirus or proxy, put its root certificate (PEM) in build/extra-ca.pem:
  npm in the container then trusts it (verification stays on).
  Usage: powershell -ExecutionPolicy Bypass -File tools\test_ui.ps1
#>
$ErrorActionPreference = 'Stop'
$env:PATH = 'C:\Program Files\Docker\Docker\resources\bin;' + $env:PATH
$root = Split-Path -Parent $PSScriptRoot
$web  = Join-Path $root 'supervisor\web'
$out  = Join-Path $root 'build\ui-screenshots'
$ca   = Join-Path $root 'build\extra-ca.pem'
New-Item -ItemType Directory -Force $out | Out-Null

python -I (Join-Path $web 'tests\make_fixture.py')

docker run --rm -v "${web}:/w:ro" -w /w node:20-alpine sh -c 'for f in *.js; do cp "$f" "/tmp/${f%.js}.mjs"; node --check "/tmp/${f%.js}.mjs" || exit 1; done; node tests/analysis.test.mjs'
if ($LASTEXITCODE -ne 0) { throw 'syntax or analysis tests failed' }

$caArgs = @()
if (Test-Path $ca) { $caArgs = @('-v', "${ca}:/ca.pem:ro", '-e', 'NODE_EXTRA_CA_CERTS=/ca.pem') }
docker run --rm @caArgs -v "${web}:/w:ro" -v "${out}:/out" mcr.microsoft.com/playwright:v1.48.2-jammy sh -c `
  'cd /w && (python3 -m http.server 8000 >/dev/null 2>&1 &) && cd /tmp && npm init -y >/dev/null 2>&1 && npm i playwright@1.48.2 >/dev/null 2>&1 && cp /w/tests/ui_smoke.mjs /tmp/ && sleep 1 && node /tmp/ui_smoke.mjs'
if ($LASTEXITCODE -ne 0) { throw 'UI smoke test failed' }
Write-Host "screenshots: $out"
