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
$stopEnd = $source.IndexOf('static void ADS_CaptureFromISR(void)', $stopStart)
if (($stopStart -lt 0) -or ($stopEnd -lt 0)) { throw 'Could not isolate ADS stop function' }
$stop = $source.Substring($stopStart, $stopEnd - $stopStart)
$sdatac = $stop.IndexOf('ADS_Command(0x11U)')
$stopCommand = $stop.IndexOf('ADS_Command(0x0AU)')
if (($sdatac -lt 0) -or ($stopCommand -lt 0) -or ($sdatac -gt $stopCommand)) {
    throw 'ADS shutdown must issue SDATAC before STOP.'
}

Write-Output 'ADS full-reset startup retry contract: PASS'
