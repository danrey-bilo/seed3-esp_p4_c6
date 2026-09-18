param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolchainBin = 'C:\Program Files\DaisyToolchain\bin'
$gitUsrBin = 'C:\Program Files\Git\usr\bin'
$gitBash = 'C:/Program Files/Git/bin/bash.exe'
$dfuSuffix = 'C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64\dfu-suffix.exe'
$localLibDaisy = Join-Path $project 'libDaisy'
$sharedLibDaisy = 'F:/Repos/seed3 audio usb/Seed3MonoUsbInput/libDaisy'

if (Test-Path -LiteralPath (Join-Path $localLibDaisy 'core/Makefile')) {
    $libDaisy = 'libDaisy'
} elseif (Test-Path -LiteralPath (Join-Path $sharedLibDaisy 'core/Makefile')) {
    # libDaisy's GNU Make files cannot safely parse a dependency path containing
    # spaces. A local ignored junction gives the build a stable, space-free path.
    New-Item -ItemType Junction -Path $localLibDaisy -Target $sharedLibDaisy | Out-Null
    $libDaisy = 'libDaisy'
} else {
    throw 'libDaisy was not found. Put it in Seed3/libDaisy or pass LIBDAISY_DIR to make.'
}

Push-Location $project
try {
    $env:Path = "$toolchainBin;$gitUsrBin;$env:Path"
    $makeArgs = @(
        "LIBDAISY_DIR=$libDaisy",
        "SHELL=$gitBash"
    )
    if ($Clean) {
        & make @makeArgs clean
        if ($LASTEXITCODE -ne 0) { throw "make clean failed: $LASTEXITCODE" }
    }
    & make @makeArgs -j8
    if ($LASTEXITCODE -ne 0) { throw "make failed: $LASTEXITCODE" }

    $firmware = Join-Path $project 'firmware'
    $sourceBin = Join-Path $project 'build\Seed3P4SpiAudio.bin'
    $readyBin = Join-Path $firmware 'Seed3P4SpiAudio.bin'
    New-Item -ItemType Directory -Force -Path $firmware | Out-Null
    Copy-Item -Force -LiteralPath $sourceBin -Destination $readyBin
    if (-not (Test-Path -LiteralPath $dfuSuffix)) {
        throw "dfu-suffix was not found: $dfuSuffix"
    }
    # The bundled dfu-util 0.10 has a known Windows suffix-layout defect.
    # ESP-IDF's dfu-suffix 0.11 writes the correct, standards-compliant suffix.
    # Add the exact STM32 ROM-DFU identity instead of bypassing that safety check.
    & $dfuSuffix --vid 0483 --pid df11 --did ffff --spec 0100 --add $readyBin
    if ($LASTEXITCODE -ne 0) { throw "dfu-suffix failed: $LASTEXITCODE" }
    $stream = [System.IO.File]::OpenRead($readyBin)
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
    "$hash  Seed3P4SpiAudio.bin" |
        Set-Content -Encoding ascii (Join-Path $firmware 'SHA256SUMS.txt')
    Write-Host "Ready-to-flash image: $readyBin"
} finally {
    Pop-Location
}
