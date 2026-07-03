$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    '#define RECORD_PROGRESS_TIMEOUT_MS\s+3000U',
    'volatile uint32_t record_progress_timeouts',
    'volatile uint32_t record_last_progress_tick',
    'volatile char record_abort_reason\[64\]',
    'session_logged_now != session_last_logged',
    'record_last_progress_tick = HAL_GetTick\(\)',
    'RECORD_PROGRESS_TIMEOUT_MS',
    'record_progress_timeouts\+\+',
    'snprintf\(\(char \*\)record_abort_reason',
    'ADS_StopAcquisition\(\)',
    'SD_RemountForLogger\(\)',
    'system_phase = 60U',
    'Recording stalled'
)

foreach ($pattern in $required) {
    if ($source -notmatch $pattern) {
        throw "Missing recording progress watchdog contract: $pattern"
    }
}

Write-Output 'Recording progress watchdog contract: PASS'
