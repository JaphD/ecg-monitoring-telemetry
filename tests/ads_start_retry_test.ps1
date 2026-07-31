$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    '#define ADS_START_MAX_ATTEMPTS\s+3U',
    'ads_start_attempts',
    'ads_start_failures',
    'ADS_ConfigureAndStartAttempt',
    'attempt <= ADS_START_MAX_ATTEMPTS',
    'HAL_SPI_DeInit\(&hspi1\)',
    'HAL_SPI_Init\(&hspi1\)'
)
foreach ($pattern in $required) {
    if ($source -notmatch $pattern) { throw "Missing ADS startup retry contract: $pattern" }
}

$stopStart = $source.IndexOf('static void ADS_StopAcquisition(void)')
$stopEnd = $source.LastIndexOf('static uint8_t ADS_CaptureFrame(void)')
if (($stopStart -lt 0) -or ($stopEnd -lt 0)) { throw 'Could not isolate ADS stop function' }
$stop = $source.Substring($stopStart, $stopEnd - $stopStart)
if ($stop.IndexOf('ADS_EnterCommandMode()') -lt 0) {
    throw 'ADS shutdown must use the verified SDATAC/STOP command-mode transition.'
}

Write-Output 'ADS full-reset startup retry contract: PASS'
