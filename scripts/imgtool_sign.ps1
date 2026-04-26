param(
  [Parameter(Mandatory = $true)][string]$InBin,
  [Parameter(Mandatory = $true)][string]$OutBin,
  [string]$Key = "",
  [string]$Version = "1.0.0",
  [string]$SlotSize = "0x60000"
)

$ErrorActionPreference = "Stop"
if (-not $Key) {
  $repo = Split-Path -Parent $PSScriptRoot
  $Key = Join-Path $repo "app\keys\mcuboot.pem"
}
if (-not (Test-Path $Key)) { throw "Missing key: $Key" }
if (-not (Test-Path $InBin)) { throw "Missing input: $InBin" }

$imgtool = Get-Command imgtool -ErrorAction Stop
& $imgtool sign --key $Key --header-size 0x200 --align 4 --version $Version --slot-size $SlotSize $InBin $OutBin
Write-Host "Signed: $OutBin"
