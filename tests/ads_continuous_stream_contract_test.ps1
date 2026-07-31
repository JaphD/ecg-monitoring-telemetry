$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    '#define ADS_CH1SET_INTERNAL_SHORT\s+0x11U',
    '#define ADS_STREAM_STATS_SAMPLES\s+500U',
    'ADS_WriteAndVerify\(ADS_REG_CH1SET,\s*ADS_CH1SET_INTERNAL_SHORT\)',
    'ADS_WriteAndVerify\(ADS_REG_CH2SET,\s*ADS_CH2SET_NORMAL\)',
    'volatile uint32_t ads_frame_error_count',
    'volatile uint32_t ads_measured_rate_millihz',
    'volatile uint32_t ads_stream_stats_sequence',
    'volatile int32_t ads_stream_ch2_pp',
    'static void ADS_UpdateStreamDiagnostics\(',
    'ads_stream_stats_sequence\+\+',
    'ADS_UpdateStreamDiagnostics\(rx, ch1, ch2\)'
)

foreach ($pattern in $required) {
    if ($source -notmatch $pattern) {
        throw "Missing continuous ADS streaming contract: $pattern"
    }
}

$captureStart = $source.LastIndexOf('static uint8_t ADS_CaptureFrame(void)')
$captureEnd = $source.IndexOf('static void ADS_Service(void)', $captureStart)
if (($captureStart -lt 0) -or ($captureEnd -lt 0)) {
    throw 'Could not isolate ADS capture path'
}
$capture = $source.Substring($captureStart, $captureEnd - $captureStart)
if ($capture.IndexOf('ADS_UpdateStreamDiagnostics(rx, ch1, ch2)') -lt 0) {
    throw 'ADS diagnostics must be updated for each successfully read frame'
}

Write-Output 'Continuous ADS streaming contract: PASS'
