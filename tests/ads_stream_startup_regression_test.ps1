$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$start = $source.IndexOf('static HAL_StatusTypeDef ADS_ConfigureAndStartAttempt(void)')
$end = $source.IndexOf('static HAL_StatusTypeDef ADS_StartAcquisition(void)', $start)
if (($start -lt 0) -or ($end -lt 0)) { throw 'Could not isolate ADS_ConfigureAndStartAttempt' }
$init = $source.Substring($start, $end - $start)

$startCommand = $init.IndexOf('ADS_Command(0x08U)')
$rdatacCommand = $init.IndexOf('ADS_Command(0x10U)')
if (($startCommand -lt 0) -or ($rdatacCommand -lt 0)) {
    throw 'ADS START/RDATAC commands are missing'
}
if ($startCommand -gt $rdatacCommand) {
    throw 'ADS startup regression: START must be sent before RDATAC'
}

$required = @(
    'ads_config1_readback = ADS_ReadReg(0x01U)',
    'ads_stream_stage = 20U',
    '__HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_9)',
    'acquisition_enabled = 1U',
    'HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_RESET',
    'ADS_CaptureFromISR()'
)
foreach ($pattern in $required) {
    if ($init.IndexOf($pattern) -lt 0) { throw "Missing ADS startup contract: $pattern" }
}

Write-Output 'ADS stream startup regression contract: PASS'
