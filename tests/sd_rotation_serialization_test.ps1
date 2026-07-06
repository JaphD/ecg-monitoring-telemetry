$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    'upload_in_progress',
    'sd_rotation_deferred',
    'sd_sync_result',
    'sd_close_result',
    'sd_rename_result',
    'upload_in_progress = 0U',
    'Logger_FinalizeRecording',
    'ADS_StopAcquisition();',
    'if (log_open && (Logger_FinalizeRecording() != FR_OK))'
)
foreach ($pattern in $required) {
    if ($source.IndexOf($pattern) -lt 0) {
        throw "Missing SD/upload serialization contract: $pattern"
    }
}

if ($source -notmatch 'sd_sync_result\s*=\s*\(uint32_t\)Logger_SyncActiveFile\(\)') {
    throw 'Logger rotation must preserve the finalization sync result.'
}
if ($source -notmatch 'sd_close_result\s*=\s*\(uint32_t\)f_close\(&log_file\)') {
    throw 'Logger rotation must preserve the f_close result.'
}
if ($source -notmatch 'sd_rename_result\s*=\s*\(uint32_t\)f_rename\(source, destination\)') {
    throw 'Logger rotation must preserve the f_rename result.'
}

Write-Output 'SD rotation and upload serialization contract: PASS'
