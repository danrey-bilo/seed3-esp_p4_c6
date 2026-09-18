param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Port,

    [int]$Baud = 115200
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $MyInvocation.MyCommand.Path
$python = 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
$image = Join-Path $project 'firmware\ESP32P4_Seed3_SPI_UAC2_2x2_Merged.bin'

if (-not (Test-Path -LiteralPath $python)) { throw "ESP-IDF Python was not found: $python" }
if (-not (Test-Path -LiteralPath $image)) { throw "Firmware image was not found: $image. Run build.cmd first." }

& $python -m esptool --chip esp32p4 --no-stub --port $Port --baud $Baud `
    --before default_reset --after hard_reset write_flash `
    --flash_mode dio --flash_freq 80m --flash_size 32MB `
    0x2000 $image
if ($LASTEXITCODE -ne 0) { throw "ESP32-P4 flashing failed: $LASTEXITCODE" }
