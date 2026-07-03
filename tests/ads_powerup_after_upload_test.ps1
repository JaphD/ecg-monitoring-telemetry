$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$start = $source.IndexOf('static HAL_StatusTypeDef ADS_ConfigureAndStartAttempt(void)')
$end = $source.IndexOf('static HAL_StatusTypeDef ADS_StartAcquisition(void)', $start)
if (($start -lt 0) -or ($end -lt 0)) { throw 'Could not isolate ADS acquisition startup' }
$init = $source.Substring($start, $end - $start)

$sequence = @(
    'HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_RESET)',
    'HAL_Delay(10U)',
    'HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_SET)',
    'HAL_Delay(2000U)',
    'HAL_Delay(100U)',
    'HAL_Delay(200U)',
    'ADS_Command(0x11U)',
    'HAL_Delay(200U)'
)
$cursor = 0
foreach ($item in $sequence) {
    $position = $init.IndexOf($item, $cursor)
    if ($position -lt 0) { throw "Missing/ordered ADS power-up step: $item" }
    $cursor = $position + $item.Length
}

if ($source -notmatch 'ADS start failed: ID=%lu') {
    throw 'Recording transition must expose the failed ADS ID.'
}

Write-Output 'ADS power-up after boot upload contract: PASS'
