$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$recordStart = $source.IndexOf('static void Run_RecordPhase(void)')
$uploadStart = $source.IndexOf('static void Run_UploadPhase(void)')
if (($recordStart -lt 0) -or ($uploadStart -lt 0) -or ($uploadStart -le $recordStart)) {
    throw 'Missing record/upload phase functions'
}

$recordText = $source.Substring($recordStart, $uploadStart - $recordStart)

if ($recordText -notmatch 'SD drain failed') {
    throw 'Record phase must expose SD drain failure in system_status.'
}

if ($recordText -match 'SD drain failed[\s\S]{0,240}Error_Handler\(\)') {
    throw 'SD drain failure must be recoverable; it must not enter Error_Handler.'
}

if ($recordText -notmatch 'sd_drain_failures') {
    throw 'SD drain failure must increment a live diagnostic counter.'
}

Write-Output 'SD drain failure recoverability contract: PASS'
