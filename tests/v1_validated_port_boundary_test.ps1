$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')
$ioc = Get-Content -Raw (Join-Path $PSScriptRoot '..\ecg monitoring telemetry.ioc')

$validatedRequired = @(
    '#include <limits\.h>',
    '#define ADS_DRDY_PORT\s+GPIOA',
    '#define ADS_DRDY_PIN\s+GPIO_PIN_0',
    '#define ADS_SETTLING_FRAMES\s+4U',
    'static HAL_StatusTypeDef ADS_EnterCommandMode\(void\)',
    'static HAL_StatusTypeDef ADS_WriteAndVerify\(uint8_t address, uint8_t value\)',
    'static HAL_StatusTypeDef ADS_StartContinuous\(void\)',
    'static uint8_t ADS_CaptureFrame\(void\)',
    'ADS_CH1SET_INTERNAL_SHORT',
    'ADS_REG_CH2SET, ADS_CH2SET_NORMAL',
    'ads_invalid_frame_drops',
    'ads_measured_rate_millihz',
    'SPI_BAUDRATEPRESCALER_256',
    'SPI_NSS_PULSE_DISABLE',
    'hsd1\.Init\.ClockDiv\s*=\s*15',
    '#define SAMPLES_PER_FILE\s+2500U',
    '#define RECORD_SESSION_MAX_MS\s+12000U',
    'STM32-%08lX%08lX%08lX',
    'Logger_NewPreambleSize\(\)'
)

foreach ($pattern in $validatedRequired) {
    if ($source -notmatch $pattern) {
        throw "Missing validated V1 port contract: $pattern"
    }
}

$iocRequired = @(
    'SDMMC1.ClockDiv=15',
    'SDMMC1.IPParameters=ClockDiv',
    'SPI1.BaudRatePrescaler=SPI_BAUDRATEPRESCALER_256',
    'SPI1.NSSPMode=SPI_NSS_PULSE_DISABLE'
)
foreach ($pattern in $iocRequired) {
    if ($ioc -notmatch [regex]::Escape($pattern)) {
        throw "CubeMX regeneration contract missing: $pattern"
    }
}

$forbidden = @(
    'ADS_DRDY_FALLBACK_MS',
    'static void ADS_CaptureFromISR\(void\)',
    'if \(GPIO_Pin == GPIO_PIN_9\)[\s\S]*ADS_Capture',
    'stm32l452-%08lX%08lX%08lX'
)
foreach ($pattern in $forbidden) {
    if ($source -match $pattern) {
        throw "Obsolete V1 acquisition/identity behavior remains: $pattern"
    }
}

$recordStart = $source.IndexOf('static void Run_RecordPhase(void)')
$uploadStart = $source.IndexOf('static void Run_UploadPhase(void)')
if (($recordStart -lt 0) -or ($uploadStart -le $recordStart)) {
    throw 'Record/upload phase boundaries were not found.'
}
$recordBody = $source.Substring($recordStart, $uploadStart - $recordStart)
$disableIndex = $recordBody.IndexOf('ModemPower_Disable("recording")')
$loggerIndex = $recordBody.IndexOf('Logger_StartRecordingWithRecovery()')
if (($disableIndex -lt 0) -or ($loggerIndex -lt 0) -or ($disableIndex -gt $loggerIndex)) {
    throw 'PB8 modem rail must be explicitly disabled before opening each recording.'
}
if ($recordBody -match 'ModemPower_Enable') {
    throw 'The recording phase must never enable the modem rail.'
}

$powerRequired = @(
    'HAL_GPIO_WritePin\(GPIOB, GPIO_PIN_8, GPIO_PIN_SET\)',
    'HAL_GPIO_WritePin\(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET\)',
    'GPIO_PIN_1 \| GPIO_PIN_2 \| GPIO_PIN_8, GPIO_PIN_RESET',
    'ModemPower_Disable\("upload complete"\)',
    'ModemPower_Disable\("TLS 715 recovery"\)',
    'tls_recovery_pending'
)
foreach ($pattern in $powerRequired) {
    if ($source -notmatch $pattern) {
        throw "V1 PB8/TLS behavior was not preserved: $pattern"
    }
}

Write-Output 'V1 validated ADS/SD/power boundary contract: PASS'
