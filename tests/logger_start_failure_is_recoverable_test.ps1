$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    'logger_start_result',
    'logger_start_failures',
    'sd_remount_attempts',
    'static FRESULT Logger_StartRecordingWithRecovery\(void\)',
    'SD_RECORD_START_RETRY_MS'
)

foreach ($pattern in $required) {
    if ($source -notmatch $pattern) {
        throw "Missing logger-start recovery contract: $pattern"
    }
}

$recordStart = $source.IndexOf('static void Run_RecordPhase(void)')
$uploadStart = $source.IndexOf('static void Run_UploadPhase(void)')
if (($recordStart -lt 0) -or ($uploadStart -lt 0)) { throw 'Missing record/upload functions' }
$recordText = $source.Substring($recordStart, $uploadStart - $recordStart)

if ($recordText -match 'Logger_StartRecording\(\) != FR_OK[\s\S]{0,120}Error_Handler\(\)') {
    throw 'Logger start failure must not immediately enter Error_Handler.'
}

if ($recordText -notmatch 'Logger_StartRecordingWithRecovery\(\)') {
    throw 'Record phase must use logger start recovery.'
}

Write-Output 'Logger start failure recoverability contract: PASS'
