$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$patterns = @(
  'SAMPLE_RING_CAPACITY\s+2048U',
  'SAMPLES_PER_FILE\s+2500U',
  'RECORD_SESSION_MAX_MS\s+12000U',
  'timestamp,accel_x,accel_y,accel_z,ecg_ch1,ecg_ch2',
  'ADS_CaptureFrame',
  'ADS_DRDY_PORT\s+GPIOA',
  'f_mount',
  'f_rename',
  'HTTP_MAX_ATTEMPTS\s+3U',
  'HTTP_RETRY_BACKOFF_MS\s+5000U',
  'AT\+HTTPACTION=1',
  'HAL_UART_Receive_IT'
)
foreach ($pattern in $patterns) {
  if ($source -notmatch $pattern) { throw "Missing production contract: $pattern" }
}
Write-Output 'SD-first telemetry rewrite contract: PASS'
