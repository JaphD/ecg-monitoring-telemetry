$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    'modem_pre_diag_sequence',
    'modem_pre_diag_ok_mask',
    'modem_pre_cfun_response',
    'modem_pre_cops_response',
    'modem_pre_csq_response',
    'modem_pre_csclk_response',
    'modem_pre_cpsi_response',
    'modem_existing_power_probe_successes',
    'modem_pwrkey_attempts',
    'modem_reset_attempts',
    'modem_forced_recovery_attempts',
    'modem_rx_start_failures',
    'modem_tx_failures',
    'modem_command_timeouts',
    'static void Modem_CapturePreRegistrationDiagnostics(void)',
    'AT+CSCLK?',
    'AT+CPSI?'
)

foreach ($item in $required) {
    if ($source -notmatch [regex]::Escape($item)) {
        throw "Missing pre-registration modem diagnostic contract: $item"
    }
}

$helperStart = $source.IndexOf('static void Modem_CapturePreRegistrationDiagnostics(void)')
$bootStart = $source.IndexOf('static uint8_t Modem_Boot(void)')
if (($helperStart -lt 0) -or ($bootStart -lt 0)) {
    throw 'Pre-registration diagnostic helper or modem boot function not found.'
}

$helperText = $source.Substring($helperStart, $bootStart - $helperStart)
$queryContracts = @(
    @{ Command = 'AT+CFUN?'; Buffer = 'modem_pre_cfun_response'; Bit = '1U << 0' },
    @{ Command = 'AT+COPS?'; Buffer = 'modem_pre_cops_response'; Bit = '1U << 1' },
    @{ Command = 'AT+CSQ'; Buffer = 'modem_pre_csq_response'; Bit = '1U << 2' },
    @{ Command = 'AT+CSCLK?'; Buffer = 'modem_pre_csclk_response'; Bit = '1U << 3' },
    @{ Command = 'AT+CPSI?'; Buffer = 'modem_pre_cpsi_response'; Bit = '1U << 4' }
)

foreach ($contract in $queryContracts) {
    $commandIndex = $helperText.IndexOf($contract.Command)
    $bufferIndex = $helperText.IndexOf($contract.Buffer)
    $bitIndex = $helperText.IndexOf($contract.Bit)
    if (($commandIndex -lt 0) -or ($bufferIndex -lt $commandIndex)) {
        throw "Response for $($contract.Command) must be frozen in $($contract.Buffer)."
    }
    if ($bitIndex -lt 0) {
        throw "Successful $($contract.Command) query must set mask bit $($contract.Bit)."
    }
}

$bootText = $source.Substring($bootStart, [Math]::Min(9000, $source.Length - $bootStart))
$ceregEnableIndex = $bootText.IndexOf('AT+CEREG=1')
$preProbeIndex = $bootText.IndexOf('Modem_CapturePreRegistrationDiagnostics();')
$registrationLoopIndex = $bootText.IndexOf('uint32_t registration_start')
if (($ceregEnableIndex -lt 0) -or ($preProbeIndex -le $ceregEnableIndex) -or
    ($registrationLoopIndex -le $preProbeIndex)) {
    throw 'The pre-registration probe must run after CEREG enable and before the registration loop.'
}

$counterContracts = @(
    'modem_existing_power_probe_successes++',
    'modem_pwrkey_attempts++',
    'modem_reset_attempts++',
    'modem_forced_recovery_attempts++',
    'modem_rx_start_failures++',
    'modem_tx_failures++',
    'modem_command_timeouts++'
)
foreach ($counter in $counterContracts) {
    if ($source -notmatch [regex]::Escape($counter)) {
        throw "Diagnostic counter is declared but not incremented: $counter"
    }
}

Write-Output 'Modem pre-registration state probe contract: PASS'
