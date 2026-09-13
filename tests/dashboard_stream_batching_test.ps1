$ErrorActionPreference = 'Stop'

$dashboardRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..\ECG_DASHBOARD')
$server = Get-Content -Raw (Join-Path $dashboardRoot 'server.ts')
$hook = Get-Content -Raw (Join-Path $dashboardRoot 'src\hooks\useTelemetryStream.ts')
$display = Get-Content -Raw (Join-Path $dashboardRoot 'src\components\TelemetryDashboard.tsx')

$required = @(
    'MAX_BATCH_SIZE',
    "app\.get\('/api/sessions/:deviceId'",
    "app\.get\('/api/telemetry/:deviceId'",
    'eq\(readings\.session_id, sessionId\)',
    'Math\.ceil\(allRows\.length / targetPoints\)',
    'selectedSessionId',
    'elapsedSeconds',
    'accelXMg',
    'unit="mg"'
)

$source = $server + $hook + $display
foreach ($pattern in $required) {
    if ($source -notmatch $pattern) {
        throw "Missing stored-session dashboard contract: $pattern"
    }
}

if ($source -match 'telemetry-batch|EventSource|m/s') {
    throw 'Dashboard must use stored session retrieval and label acceleration in mg.'
}

Write-Output 'Dashboard stored-session batching contract: PASS'
