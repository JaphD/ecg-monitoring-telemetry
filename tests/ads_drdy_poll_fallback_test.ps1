$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')
$interrupts = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\stm32l4xx_it.c')

$required = @(
    '#define ADS_DRDY_PORT\s+GPIOA',
    '#define ADS_DRDY_PIN\s+GPIO_PIN_0',
    'ads_capture_mode',
    'ads_poll_capture_count',
    'static void ADS_Service\(void\)',
    'HAL_GPIO_ReadPin\(ADS_DRDY_PORT, ADS_DRDY_PIN\) == GPIO_PIN_RESET',
    'ads_capture_mode = 2U',
    'static uint8_t ADS_CaptureFrame\(void\)',
    'static uint8_t ADS_WaitForDrdyState\(',
    'ADS_WaitForDrdyState\(GPIO_PIN_SET, 2U\)',
    'ads_invalid_frame_drops',
    'ads_drdy_release_timeouts'
)
foreach ($pattern in $required) {
    if ($source -notmatch $pattern) {
        throw "Missing ADS DRDY polling fallback contract: $pattern"
    }
}

$gpioStart = $source.IndexOf('static void MX_GPIO_Init(void)')
$gpioStart = $source.IndexOf('static void MX_GPIO_Init(void)', $gpioStart + 1)
$gpioEnd = $source.IndexOf('void Error_Handler(void)', $gpioStart)
if (($gpioStart -lt 0) -or ($gpioEnd -lt 0)) {
    throw 'Could not isolate the MX_GPIO_Init implementation.'
}
$gpio = $source.Substring($gpioStart, $gpioEnd - $gpioStart)
$drdyPin = $gpio.IndexOf('gpio.Pin = ADS_DRDY_PIN')
if ($drdyPin -lt 0) {
    throw 'PA0 DRDY must be initialized as a no-pull digital input.'
}
$drdyMode = $gpio.IndexOf('gpio.Mode = GPIO_MODE_INPUT', $drdyPin)
if ($drdyMode -lt 0) {
    throw 'PA0 DRDY must be initialized as a no-pull digital input.'
}
$drdyPull = $gpio.IndexOf('gpio.Pull = GPIO_NOPULL', $drdyMode)
if ($drdyPull -lt 0) {
    throw 'PA0 DRDY must be initialized as a no-pull digital input.'
}
$drdyInit = $gpio.IndexOf('HAL_GPIO_Init(ADS_DRDY_PORT, &gpio)', $drdyPull)
if ($drdyInit -lt 0) {
    throw 'PA0 DRDY must be initialized as a no-pull digital input.'
}

if ($source -match 'HAL_GPIO_EXTI_Callback\(uint16_t GPIO_Pin\)') {
    throw 'ADS capture must not be triggered by an unrelated EXTI callback.'
}

if ($source -match 'EXTI9_5_IRQn') {
    throw 'ADS capture must not configure or disable PB9 EXTI.'
}

if ($interrupts -match 'ADS1292R DRDY on PB9') {
    throw 'PB9 interrupt handler is incorrectly labelled as ADS DRDY.'
}

if ($source -notmatch 'total_samples_acquired\+\+;[\s\S]{0,100}total_samples_acquired\s*>=\s*2U[\s\S]{0,80}ads_stream_stage\s*=\s*100U') {
    throw 'ADS stream stage 100 must require at least two captured frames.'
}

Write-Output 'ADS PA0 polling capture contract: PASS'
