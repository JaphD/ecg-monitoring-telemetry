$ErrorActionPreference = 'Stop'
$main = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')
$disk = Get-Content -Raw (Join-Path $PSScriptRoot '..\FATFS\Target\sd_diskio.c')

$diskPatterns = @(
    'SD_RecoverCard',
    'HAL_SD_Abort(&hsd1)',
    'HAL_SD_DeInit(&hsd1)',
    'HAL_SD_Init(&hsd1)',
    'HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B)',
    'sd_driver_timeouts',
    'sd_driver_recoveries',
    'sd_driver_recovery_failures'
)
foreach ($pattern in $diskPatterns) {
    if ($disk.IndexOf($pattern) -lt 0) {
        throw "Missing SD timeout recovery contract: $pattern"
    }
}

$mainPatterns = @(
    'sd_logger_write_fault',
    'sd_last_write_result',
    'sd_write_retry_tick',
    'if (sd_logger_write_fault && ((HAL_GetTick() - sd_write_retry_tick) < 100U))',
    'sd_logger_write_fault = 0U'
)
foreach ($pattern in $mainPatterns) {
    if ($main.IndexOf($pattern) -lt 0) {
        throw "Missing SD logger fault-throttle contract: $pattern"
    }
}

Write-Output 'SD timeout recovery and logger fault throttle: PASS'
