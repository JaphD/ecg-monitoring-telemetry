$ErrorActionPreference = 'Stop'

$firmwareRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$dashboardRoot = Resolve-Path (Join-Path $firmwareRoot '..\Web-Dashboard')
$source = Get-Content -Raw (Join-Path $firmwareRoot 'Core\Src\main.c')
$server = Get-Content -Raw (Join-Path $dashboardRoot 'server.js')
$html = Get-Content -Raw (Join-Path $dashboardRoot 'dashboard.html')

$firmwarePatterns = @(
    '#define JSON_READINGS_PER_BATCH\s+250U',
    '#define SERVER_TIME_URL',
    'static uint8_t ServerTime_Sync\(void\)',
    'AT\+HTTPACTION=0',
    'AT\+HTTPREAD=0,128',
    'epoch_ms',
    'network_time_source',
    'modem_clock_failures',
    'server_time_syncs',
    'server_time_failures',
    'static uint8_t File_ComputeId\(FIL \*file, char \*file_id, size_t capacity\)',
    '2166136261U',
    '16777619U',
    '\\"file_id\\":\\"%s\\"',
    '\\"batch_index\\":%lu',
    'batch_rows >= JSON_READINGS_PER_BATCH',
    'static uint8_t HTTP_PostJsonBatchWithRetry\(',
    'upload_batch_retries\+\+',
    'current_upload_batch_index = batch_index'
)

foreach ($pattern in $firmwarePatterns) {
    if ($source -notmatch $pattern) {
        throw "Missing V1 idempotent firmware contract: $pattern"
    }
}

$serverPatterns = @(
    'app\.get\("/api/time"',
    'epoch_ms:\s*Date\.now\(\)',
    'acceptedBatches',
    'BATCH_DEDUPE_TTL_MS',
    'file_id',
    'batch_index',
    'duplicate:\s*true',
    'accepted:\s*0'
)

foreach ($pattern in $serverPatterns) {
    if ($server -notmatch $pattern) {
        throw "Missing V1 server deduplication contract: $pattern"
    }
}

if ($html -notmatch '<div class="stat-label">READINGS</div>') {
    throw 'Dashboard must label the sample counter as READINGS.'
}
if ($html -notmatch 'readings/s') {
    throw 'Dashboard rate must be labeled in readings/s.'
}
if ($html -match '<div class="stat-label">PACKETS</div>') {
    throw 'Dashboard must not call individual sample rows packets.'
}

Write-Output 'V1 idempotent upload and server-time contract: PASS'
