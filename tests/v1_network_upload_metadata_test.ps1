$ErrorActionPreference = 'Stop'

$mainPath = Join-Path $PSScriptRoot '..\Core\Src\main.c'
$source = Get-Content -Raw $mainPath

if ($source -notmatch 'Modem_ReadNetworkMetadata') {
    throw 'V1 firmware must query serving-cell metadata after LTE registration.'
}

foreach ($field in @('network_mcc', 'network_mnc', 'network_tac', 'network_cell_id')) {
    if ($source -notmatch [regex]::Escape($field)) {
        throw "V1 firmware must expose $field for hardware validation."
    }
}

foreach ($header in @('X-Network-MCC', 'X-Network-MNC', 'X-Network-TAC', 'X-Network-Cell-ID')) {
    if ($source -notmatch [regex]::Escape($header)) {
        throw "V1 firmware must send $header as optional HTTP metadata."
    }
}

if ($source -notmatch 'AT\+CPSI\?') {
    throw 'Serving-cell metadata must come from the A7670G AT+CPSI? response.'
}

if ($source -match '#define CSV_HEADER[^\r\n]*(MCC|MNC|TAC|Cell)') {
    throw 'Network metadata must not change the validated ECG CSV row contract.'
}

if ($source -match 'if\s*\(\s*!Modem_ReadNetworkMetadata\([^)]*\)\s*\)\s*\{?\s*return') {
    throw 'A failed serving-cell query must not prevent ECG uploads.'
}

if (([regex]::Matches($source, 'AT\+HTTPPARA=\\"USERDATA')).Count -ne 1) {
    throw 'Battery and network metadata must share one USERDATA command.'
}

if ($source -notmatch '\?\s*"\\\\r\\\\n"\s*:\s*""') {
    throw 'The shared USERDATA value must separate custom HTTP headers with CRLF escapes.'
}

Write-Host 'PASS: V1 serving-cell data is optional HTTP metadata and leaves the CSV contract intact.'
