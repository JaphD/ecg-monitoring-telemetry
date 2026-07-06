$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    'ads_capture_mode',
    'ads_poll_capture_count',
    'ads_last_irq_tick',
    'static void ADS_Service(void)',
    'HAL_NVIC_DisableIRQ(EXTI9_5_IRQn)',
    'HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_9) == GPIO_PIN_RESET',
    'ads_capture_mode = 2U',
    'ads_stream_stage = 30U',
    'ADS_Service();'
)
foreach ($pattern in $required) {
    if ($source.IndexOf($pattern) -lt 0) {
        throw "Missing ADS DRDY polling fallback contract: $pattern"
    }
}

if ($source -notmatch 'total_samples_acquired\+\+;[\s\S]{0,100}total_samples_acquired\s*>=\s*2U[\s\S]{0,80}ads_stream_stage\s*=\s*100U') {
    throw 'ADS stream stage 100 must require at least two captured frames.'
}

Write-Output 'ADS DRDY interrupt-to-polling fallback contract: PASS'
