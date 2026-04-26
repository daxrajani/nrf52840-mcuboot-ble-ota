$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$keyDir = Join-Path $repoRoot "app\keys"
$keyPath = Join-Path $keyDir "mcuboot.pem"

New-Item -ItemType Directory -Force -Path $keyDir | Out-Null

if (Test-Path $keyPath) {
  Write-Host "Key already exists: $keyPath"
  exit 0
}

$imgtool = Get-Command imgtool -ErrorAction SilentlyContinue
if (-not $imgtool) {
  Write-Host "imgtool not found on PATH. Install Zephyr SDK / activate nRF environment, then retry."
  Write-Host "Fallback (OpenSSL):"
  Write-Host "  openssl ecparam -name prime256v1 -genkey -noout -out `"$keyPath`""
  exit 1
}

& imgtool keygen -k $keyPath -t ecdsa-p256
Write-Host "Created $keyPath"
