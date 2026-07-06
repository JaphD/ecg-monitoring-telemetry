$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    '#define ADS_RESTART_QUIET_MS\s+',
    '#define ADS_ID_READ_MAX_ATTEMPTS\s+',
    '#define ADS_ID_READ_RETRY_MS\s+',
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
    'for \(uint32_t id_attempt = 1U; id_attempt <= ADS_ID_READ_MAX_ATTEMPTS; id_attempt\+\+\)',
    'HAL_Delay\(ADS_ID_READ_RETRY_MS\)',
    'ads_start_id_failures\+\+',
    'ads_start_config_failures\+\+',
    'ads_start_drdy_failures\+\+'
)
foreach ($pattern in $attemptPatterns) {
    if ($attemptText -notmatch $pattern) { throw "ADS configure/start attempt missing: $pattern" }
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
