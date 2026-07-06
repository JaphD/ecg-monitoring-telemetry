$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

if ($source -notmatch 'static void Drain_UploadQueueBeforeNextRecord\(void\)') {
    throw 'Missing upload-drain gate before the next recording session.'
}

$mainStart = $source.IndexOf('int main(void)')
if ($mainStart -lt 0) { throw 'Missing main function' }
$mainText = $source.Substring($mainStart)

$gateIndex = $mainText.IndexOf('Drain_UploadQueueBeforeNextRecord();')
$recordIndex = $mainText.IndexOf('Run_RecordPhase();')
if (($gateIndex -lt 0) -or ($recordIndex -lt 0) -or ($gateIndex -gt $recordIndex)) {
    throw 'main must drain any queued upload files before starting a new recording.'
}

if ($source -notmatch 'UPLOAD_RETRY_IDLE_MS') {
    throw 'Upload retry backoff must be explicit and live in the state-machine contract.'
}

if ($source -notmatch 'static uint8_t IsReadyFileName\(const char \*name\)' -or
    $source -notmatch 'IsReadyFileName\(info\.fname\)') {
    throw 'Queue counting and upload selection must use the same exact .RDY filename matcher.'
}

if ($source -notmatch 'queue_reconcile_events' -or
    $source -notmatch 'Upload_CaptureFailure\("NO_READY_FILE"\)' -or
    $source -notmatch 'Upload_CaptureFailure\("READY_STAT"\)') {
    throw 'Upload queue mismatch/f_stat failures must be visible and recoverable.'
}

Write-Output 'Upload backlog blocks next recording contract: PASS'
