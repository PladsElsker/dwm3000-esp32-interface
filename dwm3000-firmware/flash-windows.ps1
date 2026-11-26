[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM\d+$')]
    [string]$Port,

    [string]$Fqbn = "esp32:esp32:esp32c3:CDCOnBoot=cdc,JTAGAdapter=builtin",

    [string]$ArduinoCli = "arduino-cli",

    [ValidateRange(115200, 2000000)]
    [int]$Baud = 460800
)

$ErrorActionPreference = "Stop"
$BuildDirectory = Join-Path $PSScriptRoot "build"

if (-not (Test-Path -LiteralPath $BuildDirectory -PathType Container)) {
    throw "Precompiled Arduino build directory not found: $BuildDirectory"
}

if (-not (Get-Command $ArduinoCli -ErrorAction SilentlyContinue)) {
    throw "Arduino CLI executable '$ArduinoCli' was not found in PATH. Install Arduino CLI and the Espressif ESP32 board package first."
}

Write-Host "Uploading precompiled ESP32-C3 firmware to $Port..."
Write-Host "FQBN: $Fqbn"
& $ArduinoCli upload --fqbn $Fqbn --port $Port --input-dir $BuildDirectory --upload-property "upload.speed=$Baud" --verbose

if ($LASTEXITCODE -ne 0) {
    throw "arduino-cli upload failed with exit code $LASTEXITCODE."
}

Write-Host "Firmware upload completed."
