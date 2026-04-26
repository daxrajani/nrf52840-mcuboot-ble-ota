param(
  [ValidateSet("v1", "v2", "v3-crash")]
  [string]$Variant = "v1",
  [string]$Board = "nrf52840dk/nrf52840"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$config = switch ($Variant) {
  "v1" { @{ BuildDir = "build_v1"; Version = "1.0.0"; Name = "Dax_BLE_v1"; Crash = "OFF" } }
  "v2" { @{ BuildDir = "build_v2"; Version = "2.0.0"; Name = "Dax_BLE_v2"; Crash = "OFF" } }
  "v3-crash" { @{ BuildDir = "build_v3_crash"; Version = "3.0.0"; Name = "Dax_BLE_v3"; Crash = "ON" } }
}

Write-Host "Building $Variant on $Board ..."
& west build --sysbuild -b $Board -d $config.BuildDir app -- `
  -DAPP_FIRMWARE_VERSION=$($config.Version) `
  -DAPP_BLE_NAME=$($config.Name) `
  -DCRASH_DEMO=$($config.Crash)

$signed = Join-Path $repo "$($config.BuildDir)\app\zephyr\zephyr.signed.bin"
if (Test-Path $signed) {
  Write-Host "Signed image: $signed"
} else {
  Write-Warning "Signed image not found at expected location: $signed"
}
