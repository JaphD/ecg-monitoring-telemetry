$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    '#define ADS_RESTART_QUIET_MS\s+',
    '#define ADS_COMMAND_MODE_RETRIES\s+2U',
    '#define ADS_REGISTER_VERIFY_RETRIES\s+3U',
    'ads_start_total_attempts',
    'ads_start_successes',
    'ads_start_id_failures',
    'ads_start_config_failures',
    'ads_start_drdy_failures'
)
foreach ($pattern in $required) {
    if ($source -notmatch $pattern) { throw "Missing ADS start hardening contract: $pattern" }
}

$attemptStart = $source.IndexOf('static HAL_StatusTypeDef ADS_ConfigureAndStartAttempt(void)')
$startStart = $source.IndexOf('static HAL_StatusTypeDef ADS_StartAcquisition(void)', $attemptStart)
if (($attemptStart -lt 0) -or ($startStart -lt 0)) { throw 'Could not isolate ADS configure/start attempt function' }
$attemptText = $source.Substring($attemptStart, $startStart - $attemptStart)

$attemptPatterns = @(
    'ADS_EnterCommandMode\(\)',
    'ADS_WriteAndVerify\(ADS_REG_CONFIG1,\s*ADS_CONFIG1_250_SPS\)',
    'ADS_StartContinuous\(\)',
    'ads_start_id_failures\+\+',
    'ads_start_config_failures\+\+',
    'ads_start_drdy_failures\+\+'
)
foreach ($pattern in $attemptPatterns) {
    if ($attemptText -notmatch $pattern) { throw "ADS configure/start attempt missing: $pattern" }
}

$commandStart = $source.IndexOf('static HAL_StatusTypeDef ADS_EnterCommandMode(void)')
$commandEnd = $source.IndexOf('static HAL_StatusTypeDef ADS_WriteAndVerify', $commandStart)
if (($commandStart -lt 0) -or ($commandEnd -lt 0)) { throw 'Could not isolate ADS command-mode hardening' }
$commandText = $source.Substring($commandStart, $commandEnd - $commandStart)
if (($commandText -notmatch 'attempt < ADS_COMMAND_MODE_RETRIES') -or
    ($commandText -notmatch 'ads_command_mode_recoveries\+\+')) {
    throw 'ADS command-mode retries/reset recovery are missing.'
}

$startEnd = $source.IndexOf('static void ADS_StopAcquisition(void)', $startStart)
if ($startEnd -lt 0) { throw 'Could not isolate ADS_StartAcquisition' }
$startText = $source.Substring($startStart, $startEnd - $startStart)

$startPatterns = @(
    'HAL_Delay\(ADS_RESTART_QUIET_MS\)',
    'ads_start_total_attempts\+\+',
    'ads_start_successes\+\+'
)
foreach ($pattern in $startPatterns) {
    if ($startText -notmatch $pattern) { throw "ADS start acquisition missing: $pattern" }
}

Write-Output 'ADS start hardening contract: PASS'
