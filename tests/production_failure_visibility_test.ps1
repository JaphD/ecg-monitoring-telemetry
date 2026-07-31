$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$patterns = @(
  'ads_poll_capture_count',
  'ads_spi_errors',
  'ads_id_value',
  'ads_config1_readback',
  'ads_stream_stage',
  'ads_drdy_pin_state',
  'ADS_DRDY_PORT',
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
$startContinuous = $source.IndexOf('static HAL_StatusTypeDef ADS_StartContinuous(void)')
$endContinuous = $source.IndexOf('static HAL_StatusTypeDef ADS_ConfigureAndStartAttempt(void)', $startContinuous)
if (($startContinuous -lt 0) -or ($endContinuous -lt 0)) {
  throw 'Could not isolate validated ADS continuous startup.'
}
$continuous = $source.Substring($startContinuous, $endContinuous - $startContinuous)
$startCommand = $continuous.IndexOf('ADS_Command(ADS_CMD_START)')
$rdatacCommand = $continuous.IndexOf('ADS_Command(ADS_CMD_RDATAC)')
$startHelper = $init.IndexOf('ADS_StartContinuous()')
$enableAcquisition = $init.LastIndexOf('acquisition_enabled = 1U')
if (($startCommand -lt 0) -or ($rdatacCommand -lt 0) -or ($enableAcquisition -lt 0) -or
    ($startHelper -lt 0) -or ($startCommand -gt $rdatacCommand) -or
    ($startHelper -gt $enableAcquisition)) {
  throw 'ADS must use START -> RDATAC -> acquisition enable ordering.'
}
Write-Output 'Acquisition start and upload failure visibility: PASS'
