$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    'modem_diag_sequence',
    'modem_diag_cfun_response',
    'modem_diag_cops_response',
    'modem_diag_csq_response',
    'modem_diag_cereg_response',
    'modem_diag_ceer_response',
    'modem_registration_poll_count',
    'modem_cereg_stat0_count',
    'modem_cereg_stat1_count',
    'modem_cereg_stat2_count',
    'modem_cereg_stat3_count',
    'modem_cereg_stat5_count',
    'static void Modem_CaptureRegistrationDiagnostics(void)',
    'AT+CFUN?',
    'AT+COPS?',
    'AT+CSQ',
    'AT+CEREG?',
    'AT+CEER'
)

foreach ($item in $required) {
    if ($source -notmatch [regex]::Escape($item)) {
        throw "Missing modem registration diagnostic contract: $item"
    }
}

$helperStart = $source.IndexOf('static void Modem_CaptureRegistrationDiagnostics(void)')
$bootStart = $source.IndexOf('static uint8_t Modem_Boot(void)')
if (($helperStart -lt 0) -or ($bootStart -lt 0)) {
    throw 'Registration diagnostic helper or modem boot function not found.'
}

$helperText = $source.Substring($helperStart, $bootStart - $helperStart)
$queryContracts = @(
    @{ Command = 'AT+CFUN?'; Buffer = 'modem_diag_cfun_response' },
    @{ Command = 'AT+COPS?'; Buffer = 'modem_diag_cops_response' },
    @{ Command = 'AT+CSQ'; Buffer = 'modem_diag_csq_response' },
    @{ Command = 'AT+CEREG?'; Buffer = 'modem_diag_cereg_response' },
    @{ Command = 'AT+CEER'; Buffer = 'modem_diag_ceer_response' }
)

foreach ($contract in $queryContracts) {
    $commandIndex = $helperText.IndexOf($contract.Command)
    $bufferIndex = $helperText.IndexOf($contract.Buffer)
    if (($commandIndex -lt 0) -or ($bufferIndex -lt 0) -or ($bufferIndex -lt $commandIndex)) {
        throw "Response for $($contract.Command) must be copied into $($contract.Buffer)."
    }
}

$bootText = $source.Substring($bootStart, [Math]::Min(9000, $source.Length - $bootStart))
$timeoutIndex = $bootText.IndexOf('LTE_REGISTRATION_TIMEOUT')
$captureIndex = $bootText.IndexOf('Modem_CaptureRegistrationDiagnostics();')
$preserveIndex = $bootText.IndexOf('modem_boot_last_response')
if (($timeoutIndex -lt 0) -or ($captureIndex -lt 0) -or ($captureIndex -gt $timeoutIndex)) {
    throw 'The frozen diagnostic helper must run only in the LTE registration timeout branch before failure return.'
}
if (($preserveIndex -lt 0) -or ($preserveIndex -gt $captureIndex)) {
    throw 'The final CEREG response must be preserved before diagnostic commands reuse last_modem_response.'
}

if ($source -notmatch 'modem_registration_poll_count\+\+') {
    throw 'Every CEREG registration poll must be counted.'
}

Write-Output 'Modem registration timeout frozen diagnostic contract: PASS'
