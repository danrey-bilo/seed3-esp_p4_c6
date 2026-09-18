param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $MyInvocation.MyCommand.Path
$idfRoot = 'C:\Espressif\frameworks\esp-idf-v5.5.5'
$idfTools = 'C:\Espressif'
$idfPython = 'C:\Espressif\tools\idf-python\3.11.2'
$envPython = 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
$firmware = Join-Path $project 'firmware'

foreach ($required in @(
    (Join-Path $idfRoot 'export.ps1'),
    $envPython
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required ESP-IDF 5.5.5 file was not found: $required"
    }
}

$env:IDF_TOOLS_PATH = $idfTools
$env:Path = "$idfPython;$env:Path"
. (Join-Path $idfRoot 'export.ps1')

Push-Location $project
try {
    if ($Clean -and (Test-Path -LiteralPath 'build')) {
        & idf.py fullclean
        if ($LASTEXITCODE -ne 0) { throw "idf.py fullclean failed: $LASTEXITCODE" }
    }

    if (-not (Test-Path -LiteralPath 'sdkconfig')) {
        & idf.py set-target esp32p4
        if ($LASTEXITCODE -ne 0) { throw "idf.py set-target failed: $LASTEXITCODE" }
    }

    & idf.py build
    if ($LASTEXITCODE -ne 0) { throw "idf.py build failed: $LASTEXITCODE" }

    New-Item -ItemType Directory -Force -Path $firmware | Out-Null
    Copy-Item -Force 'build\bootloader\bootloader.bin' (Join-Path $firmware 'bootloader.bin')
    Copy-Item -Force 'build\partition_table\partition-table.bin' (Join-Path $firmware 'partition-table.bin')
    Copy-Item -Force 'build\seed3_p4_spi_uac2.bin' (Join-Path $firmware 'seed3_p4_spi_uac2.bin')

    $merged = Join-Path $firmware 'ESP32P4_Seed3_SPI_UAC2_2x2_Merged.bin'
    & $envPython -m esptool --chip esp32p4 merge_bin `
        --output $merged --target-offset 0x2000 `
        --flash_mode dio --flash_freq 80m --flash_size 32MB `
        0x2000 'build\bootloader\bootloader.bin' `
        0x8000 'build\partition_table\partition-table.bin' `
        0x10000 'build\seed3_p4_spi_uac2.bin'
    if ($LASTEXITCODE -ne 0) { throw "esptool merge_bin failed: $LASTEXITCODE" }

    @('bootloader.bin', 'partition-table.bin', 'seed3_p4_spi_uac2.bin',
      'ESP32P4_Seed3_SPI_UAC2_2x2_Merged.bin') |
        ForEach-Object { Get-Item -LiteralPath (Join-Path $firmware $_) } |
        Sort-Object Name |
        ForEach-Object {
            $stream = [System.IO.File]::OpenRead($_.FullName)
            try {
                $sha256 = [System.Security.Cryptography.SHA256]::Create()
                try {
                    $hash = ([System.BitConverter]::ToString($sha256.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
                } finally {
                    $sha256.Dispose()
                }
            } finally {
                $stream.Dispose()
            }
            "$hash  $($_.Name)"
        } | Set-Content -Encoding ascii (Join-Path $firmware 'SHA256SUMS.txt')

    Write-Host "Ready-to-flash images: $firmware"
} finally {
    Pop-Location
}
