$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $MyInvocation.MyCommand.Path
$dfu = 'C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64\dfu-util.exe'
$image = Join-Path $project 'firmware\Seed3P4SpiAudio.bin'

if (-not (Test-Path -LiteralPath $dfu)) { throw "dfu-util was not found: $dfu" }
if (-not (Test-Path -LiteralPath $image)) { throw "Firmware image was not found: $image. Run build.cmd first." }

function Invoke-DfuUtil {
    param([Parameter(Mandatory)][string]$Arguments)

    # Capture stderr with System.Diagnostics.Process. PowerShell otherwise turns
    # dfu-util's expected final GET_STATUS message into a terminating error when
    # $ErrorActionPreference is Stop.
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $dfu
    $startInfo.Arguments = $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw 'Could not start dfu-util.' }
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    [pscustomobject]@{
        ExitCode = $process.ExitCode
        Output = (($stdout, $stderr) -join "`n").TrimEnd()
    }
}

$flashResult = Invoke-DfuUtil "--device 0483:df11 --alt 0 --dfuse-address 0x08000000:leave --download `"$image`""
$flashOutput = $flashResult.Output
$flashCode = $flashResult.ExitCode
$flashOutput | ForEach-Object { Write-Host $_ }

if ($flashCode -ne 0) {
    # STM32H7 ROM disconnects immediately after the DfuSe LEAVE request and
    # dfu-util can then fail its final GET_STATUS. Accept that one specific
    # case only when the completed download is reported and 0483:df11 is gone.
    $flashText = $flashOutput -join "`n"
    Start-Sleep -Milliseconds 750
    $listOutput = (Invoke-DfuUtil '--list').Output
    $leftDfu = $listOutput -notmatch '0483:df11'
    if ($flashCode -ne 74 -or
        $flashText -notmatch 'File downloaded successfully' -or
        -not $leftDfu) {
        throw "Daisy Seed3 flashing failed: $flashCode"
    }
    Write-Host 'Seed3 left ROM DFU immediately after the successful download.'
}
Write-Host 'Seed3 firmware written successfully and started.'
