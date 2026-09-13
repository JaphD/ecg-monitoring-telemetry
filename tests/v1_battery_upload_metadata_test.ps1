$ErrorActionPreference = 'Stop'

$mainPath = Join-Path $PSScriptRoot '..\Core\Src\main.c'
$source = Get-Content -Raw $mainPath

if ($source -notmatch 'AT\+CBC') {
    throw 'V1 firmware must query the A7670G supply voltage with AT+CBC.'
}

if ($source -notmatch 'X-Battery-Millivolts') {
    throw 'V1 firmware must send battery voltage as optional HTTP metadata.'
}

if ($source -notmatch 'battery_voltage_mv') {
    throw 'V1 firmware must expose the last parsed battery voltage for hardware validation.'
}

if ($source -match '#define CSV_HEADER[^\r\n]*battery') {
    throw 'Battery metadata must not change the validated ECG CSV row contract.'
}

if ($source -match 'if\s*\(\s*!Modem_ReadBatteryVoltage\([^)]*\)\s*\)\s*\{?\s*return') {
    throw 'A failed battery query must not prevent ECG uploads.'
}

Write-Host 'PASS: V1 battery voltage is optional HTTP metadata and leaves the CSV contract intact.'
