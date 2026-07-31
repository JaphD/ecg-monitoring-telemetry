$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$start = $source.IndexOf('static HAL_StatusTypeDef ADS_ConfigureAndStartAttempt(void)')
$end = $source.IndexOf('static HAL_StatusTypeDef ADS_StartAcquisition(void)', $start)
if (($start -lt 0) -or ($end -lt 0)) { throw 'Could not isolate ADS_ConfigureAndStartAttempt' }
$init = $source.Substring($start, $end - $start)

$continuousStart = $source.IndexOf('static HAL_StatusTypeDef ADS_StartContinuous(void)')
$continuousEnd = $source.IndexOf('static HAL_StatusTypeDef ADS_ConfigureAndStartAttempt(void)', $continuousStart)
if (($continuousStart -lt 0) -or ($continuousEnd -lt 0)) { throw 'Could not isolate ADS_StartContinuous' }
$continuous = $source.Substring($continuousStart, $continuousEnd - $continuousStart)
$startCommand = $continuous.IndexOf('ADS_Command(ADS_CMD_START)')
$rdatacCommand = $continuous.IndexOf('ADS_Command(ADS_CMD_RDATAC)')
if (($startCommand -lt 0) -or ($rdatacCommand -lt 0)) {
    throw 'ADS START/RDATAC commands are missing'
}
if ($startCommand -gt $rdatacCommand) {
    throw 'ADS startup regression: START must be sent before RDATAC'
}

$required = @(
    'ADS_WriteAndVerify(ADS_REG_CONFIG1, ADS_CONFIG1_250_SPS)',
    'ADS_StartContinuous()',
    'ads_stream_stage = 20U',
    'acquisition_enabled = 1U',
    'ads_capture_mode = 2U',
    'ADS_Service()'
)
foreach ($pattern in $required) {
    if ($init.IndexOf($pattern) -lt 0) { throw "Missing ADS startup contract: $pattern" }
}

Write-Output 'ADS stream startup regression contract: PASS'
