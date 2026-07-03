$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$patterns = @(
    '#define RECORD_SESSION_MAX_MS\s+12000U',
    '#define SAMPLES_PER_FILE\s+2500U',
    '#define ADS_CONFIG1_250_SPS\s+0x01U',
    'system_phase',
    'record_sessions_completed',
    'upload_phases_completed',
    'static void Logger_RecoverQueue\(void\)',
    'static FRESULT Logger_StartRecording\(void\)',
    'static FRESULT Logger_FinalizeRecording\(void\)',
    'static HAL_StatusTypeDef ADS_StartAcquisition\(void\)',
    'static void ADS_StopAcquisition\(void\)',
    'static void Run_RecordPhase\(void\)',
    'static void Run_UploadPhase\(void\)',
    'system_phase = 20U',
    'system_phase = 40U'
)
foreach ($pattern in $patterns) {
    if ($source -notmatch $pattern) { throw "Missing record/upload state contract: $pattern" }
}

$uploadStart = $source.IndexOf('static void Run_UploadPhase(void)')
$mainStart = $source.IndexOf('int main(void)')
if (($uploadStart -lt 0) -or ($mainStart -lt 0)) { throw 'Missing state-machine functions' }
$uploadText = $source.Substring($uploadStart, $mainStart - $uploadStart)
if ($uploadText -notmatch 'ADS_StopAcquisition\(\)') {
    throw 'Upload phase must explicitly stop ADS acquisition.'
}
if ($uploadText -notmatch 'Logger_FinalizeRecording\(\)') {
    throw 'Upload phase must finalize and close the active logger.'
}

$recordStart = $source.IndexOf('static void Run_RecordPhase(void)')
if ($recordStart -lt 0) { throw 'Missing record phase function' }
$recordText = $source.Substring($recordStart, $uploadStart - $recordStart)
if ($recordText -notmatch 'session_start_logged = total_samples_logged' -or
    $recordText -notmatch '\(total_samples_logged - session_start_logged\) < SAMPLES_PER_FILE') {
    throw 'Record phase must stop by fixed row count, not only elapsed time.'
}

$mainText = $source.Substring($mainStart)
$bootUpload = $mainText.IndexOf('Drain_UploadQueueBeforeNextRecord();')
$record = $mainText.IndexOf('Run_RecordPhase();')
if (($bootUpload -lt 0) -or ($record -lt 0) -or ($bootUpload -gt $record)) {
    throw 'Boot must drain the recovered queue before the first recording phase.'
}

Write-Output 'Record/upload phase state machine contract: PASS'
