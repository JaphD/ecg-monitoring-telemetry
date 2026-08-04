$ErrorActionPreference = 'Stop'

$dashboardRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..\Web-Dashboard')
$server = Get-Content -Raw (Join-Path $dashboardRoot 'server.js')
$html = Get-Content -Raw (Join-Path $dashboardRoot 'dashboard.html')
$parser = Get-Content -Raw (Join-Path $dashboardRoot 'telemetry-parser.js')

$serverPatterns = @(
    'createUploadParser',
    'broadcastTelemetryBatch',
    'broadcastSSE\("telemetry-batch"',
    'part_index',
    'part_count',
    'res\.status\(result\.status\)'
)

foreach ($pattern in $serverPatterns) {
    if ($server -notmatch $pattern) {
        throw "Missing server batching contract: $pattern"
    }
}

$htmlPatterns = @(
    'uploadAssemblies',
    'acceptBatchPart',
    'renderCompleteUpload',
    'addEventListener\("telemetry-batch"',
    '<div class="stat-unit">mg</div>',
    '<div class="label">mg</div>'
)

foreach ($pattern in $htmlPatterns) {
    if ($html -notmatch $pattern) {
        throw "Missing dashboard throttling contract: $pattern"
    }
}

if ($html -match 'let activeDeviceId' -or
    $html -match 'activeDeviceId\s*&&\s*device\.id') {
    throw 'Dashboard must not remain locked to the first device UID in the browser session.'
}

if ($html -notmatch 'assembly\.deviceId\s*!==\s*device\.id') {
    throw 'Dashboard must retain per-upload device-ID consistency validation.'
}

if ($html -match 'm/s') {
    throw 'Dashboard must label LIS3DH acceleration as mg, not m/s^2.'
}

$removedIdentityPatterns = @(
    'ECG_MONITOR_01_UID',
    'buildAliases',
    'Unregistered ECG Monitor',
    'UID UNREGISTERED'
)

foreach ($pattern in $removedIdentityPatterns) {
    if (($server + $html + $parser) -match $pattern) {
        throw "Friendly-name registration behavior must be removed: $pattern"
    }
}

Write-Output 'Dashboard identified SSE batching contract: PASS'
