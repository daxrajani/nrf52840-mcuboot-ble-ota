param(
  [Parameter(Mandatory = $true)][string]$Image,
  [Parameter(Mandatory = $true)][string]$Address
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Image)) {
  throw "Image file does not exist: $Image"
}

Write-Host "Flashing secondary slot image..."
Write-Host "  image: $Image"
Write-Host "  addr : $Address"

& nrfjprog --program $Image --sectorerase --verify -f nrf52 --addr $Address
Write-Host "Done. Reset board and observe MCUboot swap logs."
