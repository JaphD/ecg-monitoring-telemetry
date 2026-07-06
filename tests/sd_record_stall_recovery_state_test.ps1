$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$requiredDiagnostics = @(
    'sd_recovery_required',
    'sd_recovery_stage',
    'sd_record_stall_recoveries',
    'sd_active_abandoned',
    'Handle_SDRecordStall',
    'Run_SDRecoveryIfNeeded'
)
foreach ($pattern in $requiredDiagnostics) {
    if ($source -notmatch $pattern) { throw "Missing SD record-stall recovery diagnostic/handler: $pattern" }
}

$recordStart = $source.IndexOf('static void Run_RecordPhase(void)')
$uploadStart = $source.IndexOf('static void Run_UploadPhase(void)', $recordStart)
if (($recordStart -lt 0) -or ($uploadStart -lt 0)) { throw 'Could not isolate Run_RecordPhase' }
$recordText = $source.Substring($recordStart, $uploadStart - $recordStart)

if ($recordText -notmatch 'Handle_SDRecordStall\(\)') {
    throw 'Run_RecordPhase must delegate record progress timeout to SD stall handler.'
}
if ($recordText -match 'Recording stalled; SD recovery[\s\S]*SD_RemountForLogger\(\)') {
    throw 'Record-stall branch must not synchronously remount SD inside active recording path.'
}

$handlerStart = $source.LastIndexOf('static void Handle_SDRecordStall(void)')
$recoveryStart = $source.IndexOf('static void Run_SDRecoveryIfNeeded(void)', $handlerStart)
if (($handlerStart -lt 0) -or ($recoveryStart -lt 0)) { throw 'Could not isolate Handle_SDRecordStall' }
$handlerText = $source.Substring($handlerStart, $recoveryStart - $handlerStart)

$handlerPatterns = @(
    'system_phase = 60U',
    'sd_record_stall_recoveries\+\+',
    'sd_recovery_required = 1U',
    'sd_recovery_stage = 10U',
    'ADS_StopAcquisition\(\)',
    'ring_tail = ring_head'
)
foreach ($pattern in $handlerPatterns) {
    if ($handlerText -notmatch $pattern) { throw "SD record-stall handler missing: $pattern" }
}

$mainStart = $source.IndexOf('while (1)')
if ($mainStart -lt 0) { throw 'Could not find main loop' }
$mainText = $source.Substring($mainStart)
if ($mainText -notmatch 'Run_SDRecoveryIfNeeded\(\)[\s\S]*Drain_UploadQueueBeforeNextRecord\(\)') {
    throw 'Main loop must run SD recovery before queue drain / next recording.'
}

Write-Output 'SD record-stall recovery state contract: PASS'
