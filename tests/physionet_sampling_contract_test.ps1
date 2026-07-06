$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    '#define ADS_CONFIG1_250_SPS\s+0x01U',
    '#define ADS_DRDY_FALLBACK_MS\s+40U',
    '#define ADS_POLL_INTERVAL_MS\s+4U',
    'ADS_WriteReg\(0x01U,\s*ADS_CONFIG1_250_SPS\)',
    'ads_config1_readback != ADS_CONFIG1_250_SPS',
    '#define RECORD_SESSION_MAX_MS\s+12000U',
    '#define SAMPLES_PER_FILE\s+2500U',
    '\(total_samples_logged - session_start_logged\) < SAMPLES_PER_FILE',
    '\(HAL_GetTick\(\) - record_start\) < RECORD_SESSION_MAX_MS',
    '\(now - ads_last_irq_tick\) >= ADS_DRDY_FALLBACK_MS',
    '\(now - ads_last_poll_tick\) >= ADS_POLL_INTERVAL_MS',
    'if \(\(now - last_imu_tick\) < 20U\) return;'
)

foreach ($pattern in $required) {
    if ($source -notmatch $pattern) {
        throw "Missing PhysioNet sampling contract: $pattern"
    }
}

if ($source -match '/\* 500 SPS \*/') {
    throw 'ADS CONFIG1 must no longer document/use 500 SPS for PhysioNet playback.'
}

Write-Output 'PhysioNet sampling contract: PASS'
