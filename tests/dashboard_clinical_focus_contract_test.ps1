$ErrorActionPreference = 'Stop'
$dashboard = Get-Content -Raw (Join-Path $PSScriptRoot '..\..\Web-Dashboard\dashboard.html')

$required = @(
    'id="deviceName"',
    'id="deviceUid"',
    'id="lastUploadTime"',
    'class="clinical-layout"',
    'class="ecg-primary-panel"',
    'id="ch2Min"',
    'id="ch2Max"',
    'id="ch2Mean"',
    'id="ch2Pp"',
    'id="observedRate"',
    'function acceptBatchPart\(envelope\)',
    'function renderCompleteUpload\(envelope, rows\)',
    'addEventListener\("telemetry-batch"',
    'decimation:\s*\{\s*enabled:\s*false',
    'tension:\s*0',
    'pointRadius:\s*0',
    '\.chart-area\s*\{[^}]*min-width:\s*0',
    'canvas\s*\{[^}]*max-width:\s*100%\s*!important',
    '\.supporting-row\s*\{[^}]*min-width:\s*0'
)

foreach ($pattern in $required) {
    if ($dashboard -notmatch $pattern) {
        throw "Missing Clinical Focus dashboard contract: $pattern"
    }
}

$primary = $dashboard.IndexOf('class="ecg-primary-panel"')
$supporting = $dashboard.IndexOf('id="panel-ecg1"')
if (($primary -lt 0) -or ($supporting -le $primary)) {
    throw 'Channel 2 must appear before supporting Channel 1 content.'
}

if ($dashboard -match 'tension:\s*0\.3') {
    throw 'ECG fidelity requires unsmoothed linear rendering.'
}

Write-Output 'Clinical Focus dashboard contract: PASS'
