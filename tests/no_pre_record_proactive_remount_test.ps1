$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$drainStart = $source.IndexOf('static void Drain_UploadQueueBeforeNextRecord(void)')
$mainStart = $source.IndexOf('int main(void)')
if (($drainStart -lt 0) -or ($mainStart -lt 0) -or ($mainStart -le $drainStart)) {
    throw 'Missing upload-drain gate or main function.'
}

$drainText = $source.Substring($drainStart, $mainStart - $drainStart)
if ($drainText -match 'PrepareStorageForNextRecord\(\)') {
    throw 'Upload-drain gate must not proactively remount SD before recording.'
}

if ($source -notmatch 'Logger_StartRecordingWithRecovery\(\)') {
    throw 'Reactive logger-start recovery must remain in place.'
}

Write-Output 'No pre-record proactive remount contract: PASS'
