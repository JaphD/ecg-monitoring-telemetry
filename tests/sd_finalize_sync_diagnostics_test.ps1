$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    'volatile uint32_t sd_sync_count',
    'volatile uint32_t sd_sync_failures',
    'volatile uint32_t sd_last_sync_tick',
    'volatile uint32_t sd_last_sync_duration_ms',
    'static FRESULT Logger_SyncActiveFile\(void\)',
    'sd_sync_count\+\+',
    'uint32_t sync_start = HAL_GetTick\(\)',
    'sd_last_sync_tick = sync_start',
    'sd_last_sync_duration_ms = HAL_GetTick\(\) - sync_start',
    'sd_sync_failures\+\+',
    'Logger_SyncActiveFile\(\)'
)

foreach ($pattern in $required) {
    if ($source -notmatch $pattern) {
        throw "Missing finalization sync diagnostics contract: $pattern"
    }
}

$drainStart = $source.IndexOf('static void Logger_Drain(uint32_t maximum_rows)')
$scanStart = $source.IndexOf('static void Logger_ScanQueue(void)')
if (($drainStart -lt 0) -or ($scanStart -lt 0) -or ($scanStart -le $drainStart)) {
    throw 'Missing Logger_Drain or Logger_ScanQueue functions'
}
$drainText = $source.Substring($drainStart, $scanStart - $drainStart)
if ($drainText -match 'f_sync\(&log_file\)') {
    throw 'Logger_Drain must not contain mid-record f_sync.'
}

Write-Output 'SD finalization sync diagnostics contract: PASS'
