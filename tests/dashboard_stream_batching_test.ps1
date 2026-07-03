$ErrorActionPreference = 'Stop'

$dashboardRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..\Web-Dashboard')
$server = Get-Content -Raw (Join-Path $dashboardRoot 'server.js')
$html = Get-Content -Raw (Join-Path $dashboardRoot 'dashboard.html')

$serverPatterns = @(
    'SSE_BATCH_INTERVAL_MS',
    'pendingRows',
    'queueSSE',
    'flushSSEBatch',
    'broadcastSSE\("sensor-batch"'
)

foreach ($pattern in $serverPatterns) {
    if ($server -notmatch $pattern) {
        throw "Missing server batching contract: $pattern"
    }
}

$htmlPatterns = @(
    'RENDER_INTERVAL_MS',
    'pendingRows',
    'flushPendingRows',
    'requestAnimationFrame',
    'addEventListener\("sensor-batch"',
    'updateCharts',
    '<div class="stat-unit">mg</div>',
    '<div class="label">mg</div>'
)

foreach ($pattern in $htmlPatterns) {
    if ($html -notmatch $pattern) {
        throw "Missing dashboard throttling contract: $pattern"
    }
}

$pushStart = $html.IndexOf('function pushPoint(chart, label, ...vals)')
$pushEnd = $html.IndexOf('function updateCharts()', $pushStart)
if (($pushStart -lt 0) -or ($pushEnd -lt 0) -or ($pushEnd -le $pushStart)) {
    throw 'Unable to locate pushPoint function body.'
}
$pushBody = $html.Substring($pushStart, $pushEnd - $pushStart)
if ($pushBody -match 'chart\.update\("none"\)') {
    throw 'pushPoint must not call Chart.js update once per row.'
}

if ($html -match 'm/s') {
    throw 'Dashboard must label LIS3DH acceleration as mg, not m/s^2.'
}

Write-Output 'Dashboard SSE batching/render throttling contract: PASS'
