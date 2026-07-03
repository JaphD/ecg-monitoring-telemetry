$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$drainStart = $source.IndexOf('static void Logger_Drain(uint32_t maximum_rows)')
$scanStart = $source.IndexOf('static void Logger_ScanQueue(void)')
if (($drainStart -lt 0) -or ($scanStart -lt 0) -or ($scanStart -le $drainStart)) {
    throw 'Missing Logger_Drain or Logger_ScanQueue functions'
}

$drainText = $source.Substring($drainStart, $scanStart - $drainStart)
if ($drainText -match 'Logger_Rotate\(\)') {
    throw 'Logger_Drain must not rotate/close/reopen the SD file during active recording.'
}

if ($drainText -match 'log_rows\s*>=\s*SAMPLES_PER_FILE') {
    throw 'Logger_Drain must not stop progress at the 2500-row file boundary.'
}

if ($drainText -match 'f_sync\(&log_file\)') {
    throw 'Logger_Drain must not call f_sync during active recording; sync only during finalization.'
}

Write-Output 'No mid-record SD rotation contract: PASS'
