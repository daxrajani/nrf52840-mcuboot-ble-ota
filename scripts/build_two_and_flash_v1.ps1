# After changing firmware: sysbuild v1 to `build/`, v2 to `build_v2/`, then program `build/merged.hex`
# (MCUboot + slot-0 app). Requires west, NCS env, and nrfjprog on PATH.
#
# From repo root:
#   .\scripts\build_two_and_flash_v1.ps1
# Clean rebuilds:
#   .\scripts\build_two_and_flash_v1.ps1 -Pristine
# Build only (no flash):
#   .\scripts\build_two_and_flash_v1.ps1 -SkipFlash

param(
	[string] $Board = "nrf52840dk/nrf52840",
	[string] $V1BuildDir = "build",
	[string] $V1Version = "1.0.0",
	[string] $V1Name = "Dax_BLE_v1",
	[string] $V2BuildDir = "build_v2",
	[string] $V2Version = "2.0.0",
	[string] $V2Name = "Dax_BLE_v2",
	[switch] $Pristine,
	[switch] $SkipFlash
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repo
Write-Host "Repository: $repo"

if (-not $env:ZEPHYR_BASE) {
	$zepCandidates = @(
		"C:\ncs\v3.2.4\zephyr",
		"C:\ncs\v2.6.0\zephyr"
	)
	if ($env:USERPROFILE) { $zepCandidates += (Join-Path $env:USERPROFILE "ncs\v3.2.4\zephyr") }
	$zepPick = $zepCandidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
	if ($zepPick) {
		$env:ZEPHYR_BASE = $zepPick
		Write-Host "ZEPHYR_BASE=$($env:ZEPHYR_BASE) (auto-set)"
	} else {
		Write-Warning "ZEPHYR_BASE is not set. Set it to your NCS Zephyr, e.g. C:\ncs\v3.2.4\zephyr"
	}
} else {
	Write-Host "ZEPHYR_BASE=$($env:ZEPHYR_BASE)"
}

if (-not $env:ZEPHYR_SDK_INSTALL_DIR) {
	$sdk = Get-ChildItem "C:\ncs\toolchains" -Directory -ErrorAction SilentlyContinue | ForEach-Object {
		Join-Path $_.FullName "opt\zephyr-sdk"
	} | Where-Object { Test-Path $_ } | Select-Object -First 1
	if ($sdk) {
		$env:ZEPHYR_SDK_INSTALL_DIR = $sdk
		Write-Host "ZEPHYR_SDK_INSTALL_DIR=$($env:ZEPHYR_SDK_INSTALL_DIR) (auto-set)"
	}
}

function Invoke-NcsWest {
	param([string[]] $ArgList)
	if (Get-Command west -ErrorAction SilentlyContinue) {
		& west @ArgList
		return
	}
	$py = Get-ChildItem "C:\ncs\toolchains" -Recurse -Filter "python.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
	if (-not $py) { throw "west is not in PATH and NCS python was not found under C:\ncs\toolchains" }
	& $py.FullName -m west @ArgList
}

if (-not (Test-Path (Join-Path $repo "app\keys\mcuboot.pem"))) {
	throw "Missing app\keys\mcuboot.pem - run scripts\setup_dev_keys.ps1 first."
}

$westP = @()
if ($Pristine) { $westP = @("-p", "always") }

$westArgsV1 = @("build", "--sysbuild", "-b", $Board, "-d", $V1BuildDir) + $westP + @(
	"app", "--",
	"-DAPP_FIRMWARE_VERSION=$V1Version",
	"-DAPP_BLE_NAME=$V1Name"
)

Write-Host "`n=== [1/2] Sysbuild v1 -> $V1BuildDir (version $V1Version) ===" -ForegroundColor Cyan
Invoke-NcsWest $westArgsV1

$westArgsV2 = @("build", "--sysbuild", "-b", $Board, "-d", $V2BuildDir) + $westP + @(
	"app", "--",
	"-DAPP_FIRMWARE_VERSION=$V2Version",
	"-DAPP_BLE_NAME=$V2Name"
)

Write-Host "`n=== [2/2] Sysbuild v2 -> $V2BuildDir (version $V2Version) ===" -ForegroundColor Cyan
Invoke-NcsWest $westArgsV2

$signed1 = Join-Path $repo "$V1BuildDir\app\zephyr\zephyr.signed.bin"
$signed2 = Join-Path $repo "$V2BuildDir\app\zephyr\zephyr.signed.bin"
$merged  = Join-Path $repo "$V1BuildDir\merged.hex"
Write-Host "`nOutputs:" -ForegroundColor Green
if (Test-Path $signed1) { Write-Host "  v1 signed: $signed1" } else { Write-Warning "  Missing: $signed1" }
if (Test-Path $signed2) { Write-Host "  v2 signed: $signed2" } else { Write-Warning "  Missing: $signed2" }
if (Test-Path $merged)  { Write-Host "  v1 merge:  $merged" }  else { Write-Warning "  Missing: $merged" }

if ($SkipFlash) {
	Write-Host "`n-SkipFlash: not programming the board." -ForegroundColor Yellow
	exit 0
}

if (-not (Test-Path $merged)) { throw "No merged image at $merged - build failed or wrong build dir." }
if (-not (Get-Command nrfjprog -ErrorAction SilentlyContinue)) { throw "nrfjprog not in PATH (install nRF command line tools)." }

Write-Host "`n=== Flashing v1 (MCUboot + app from $V1BuildDir) ===" -ForegroundColor Cyan
& nrfjprog --program $merged -f nrf52 --sectorerase --verify --reset
if ($LASTEXITCODE -ne 0) {
	throw "nrfjprog failed (exit $LASTEXITCODE). Connect the nRF52840 DK (USB) and J-Link, or run with -SkipFlash if you will flash manually."
}
Write-Host "Done. Use v2 for OTA: $signed2" -ForegroundColor Green