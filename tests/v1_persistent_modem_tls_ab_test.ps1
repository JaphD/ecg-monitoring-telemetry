$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$requiredDiagnostics = @(
    '#define MODEM_HOLD_POWER_AB_TEST 1U',
    'modem_hold_power_ab_test',
    'http_attempts_total',
    'http_status_200_count',
    'http_status_715_count',
    'http_status_422_count',
    'http_status_other_count',
    'httpaction_wait_failures',
    'httpterm_failures',
    'httpinit_failures',
    'http_success_attempt1_count',
    'http_success_attempt2_count',
    'http_success_attempt3_count',
    'http_last_attempt_duration_ms',
    'http_last_action_duration_ms',
    'http_last_failure_status',
    'http_last_failure_attempt',
    'http_last_failure_duration_ms',
    'http_last_failure_power_age_ms',
    'http_last_failure_step',
    'http_last_failure_response'
)

foreach ($item in $requiredDiagnostics) {
    if ($source -notmatch [regex]::Escape($item)) {
        throw "Missing persistent-modem A/B diagnostic contract: $item"
    }
}

$recordStart = $source.IndexOf('static void Run_RecordPhase(void)')
$uploadStart = $source.IndexOf('static void Run_UploadPhase(void)')
$drainStart = $source.IndexOf('static void Drain_UploadQueueBeforeNextRecord(void)')
if (($recordStart -lt 0) -or ($uploadStart -le $recordStart) -or ($drainStart -le $uploadStart)) {
    throw 'Unable to isolate record, upload, and queue-drain functions.'
}

$recordText = $source.Substring($recordStart, $uploadStart - $recordStart)
$uploadText = $source.Substring($uploadStart, $drainStart - $uploadStart)
$drainText = $source.Substring($drainStart, 3500)

if ($recordText -notmatch '#if MODEM_HOLD_POWER_AB_TEST == 0U[\s\S]*ModemPower_Disable\("recording"\)[\s\S]*#endif') {
    throw 'Recording must retain modem power while the A/B test is enabled.'
}
if ($uploadText -notmatch '#if MODEM_HOLD_POWER_AB_TEST == 0U[\s\S]*ModemPower_Disable\("upload complete"\)[\s\S]*#endif') {
    throw 'Successful uploads must retain modem power while the A/B test is enabled.'
}
if ($drainText -notmatch 'ModemPower_Disable\("TLS 715 recovery"\)') {
    throw 'An exhausted TLS 715 round must retain the existing rail-cycle recovery.'
}

$httpStart = $source.IndexOf('static uint8_t HTTP_PostFile(')
$oldestStart = $source.IndexOf('static void Upload_OldestReady(void)')
if (($httpStart -lt 0) -or ($oldestStart -le $httpStart)) {
    throw 'Unable to isolate HTTP post and upload-attempt functions.'
}

$httpText = $source.Substring($httpStart, $oldestStart - $httpStart)
$oldestText = $source.Substring($oldestStart, $recordStart - $oldestStart)

$httpRequirements = @(
    'httpterm_failures++',
    'httpinit_failures++',
    'httpaction_wait_failures++',
    'http_status_200_count++',
    'http_status_715_count++',
    'http_status_422_count++',
    'http_status_other_count++',
    'http_last_action_duration_ms'
)
foreach ($item in $httpRequirements) {
    if ($httpText -notmatch [regex]::Escape($item)) {
        throw "HTTP transaction does not update diagnostic: $item"
    }
}

if ($oldestText -notmatch 'http_attempts_total\+\+[\s\S]*HTTP_PostFile\(path\)') {
    throw 'Each real HTTP post attempt must increment the total-attempt denominator.'
}
if ($oldestText -notmatch 'http_last_failure_status = current_http_status[\s\S]*http_last_failure_step[\s\S]*http_last_failure_response') {
    throw 'A failed attempt must remain inspectable after a later retry succeeds.'
}
if ($oldestText -notmatch 'case 1U:[\s\S]*http_success_attempt1_count\+\+[\s\S]*case 2U:[\s\S]*http_success_attempt2_count\+\+[\s\S]*case 3U:[\s\S]*http_success_attempt3_count\+\+') {
    throw 'Successful uploads must be classified by the attempt that recovered them.'
}

Write-Output 'V1 persistent modem TLS A/B contract: PASS'
