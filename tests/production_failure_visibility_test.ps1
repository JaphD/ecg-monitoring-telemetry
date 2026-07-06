$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$patterns = @(
  'ads_drdy_irq_count',
  'ads_spi_errors',
  'ads_id_value',
  'ads_config1_readback',
  'ads_stream_stage',
  'ads_drdy_pin_state',
  '__HAL_GPIO_EXTI_CLEAR_IT\(GPIO_PIN_9\)',
  'upload_failure_step',
  'upload_failure_response',
  'Upload_CaptureFailure'
)
foreach ($pattern in $patterns) {
  if ($source -notmatch $pattern) { throw "Missing failure diagnostic: $pattern" }
}

$initStart = $source.IndexOf('static HAL_StatusTypeDef ADS_ConfigureAndStartAttempt(void)')
$initEnd = $source.IndexOf('static HAL_StatusTypeDef ADS_StartAcquisition(void)', $initStart)
$init = $source.Substring($initStart, $initEnd - $initStart)
$startCommand = $init.IndexOf('ADS_Command(0x08U)')
$rdatacCommand = $init.IndexOf('ADS_Command(0x10U)')
$enableAcquisition = $init.LastIndexOf('acquisition_enabled = 1U')
if (($startCommand -lt 0) -or ($rdatacCommand -lt 0) -or ($enableAcquisition -lt 0) -or
    ($startCommand -gt $rdatacCommand) -or ($rdatacCommand -gt $enableAcquisition)) {
  throw 'ADS must use START -> RDATAC -> acquisition enable ordering.'
}
Write-Output 'Acquisition start and upload failure visibility: PASS'
