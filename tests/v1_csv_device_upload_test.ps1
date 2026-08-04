$ErrorActionPreference = 'Stop'

$firmware = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')
$server = Get-Content -Raw (Join-Path $PSScriptRoot '..\..\Web-Dashboard\server.js')
$parser = Get-Content -Raw (Join-Path $PSScriptRoot '..\..\Web-Dashboard\telemetry-parser.js')

$firmwareRequired = @(
    '#define SAMPLES_PER_FILE\s+2500U',
    '#define MAX_HTTPDATA_BYTES\s+100000U',
    'STM32-%08lX%08lX%08lX',
    'device_id,%s\\r\\n',
    'static uint8_t HTTP_PostFile\(const char \*path\)',
    'AT\+HTTPPARA=\\"CONTENT\\",\\"text/csv\\"',
    'uint8_t chunk\[512\]',
    'f_read\(&upload, chunk, sizeof\(chunk\), &bytes_read\)',
    'HAL_UART_Transmit\(&huart1, chunk, \(uint16_t\)bytes_read, 3000U\)',
    'if \(HTTP_PostFile\(path\)\)',
    'static uint8_t ModemPower_BootForUpload\(void\)',
    'HAL_GPIO_WritePin\(GPIOB, GPIO_PIN_8, GPIO_PIN_SET\)'
)

foreach ($pattern in $firmwareRequired) {
    if ($firmware -notmatch $pattern) {
        throw "Missing V1 CSV upload contract: $pattern"
    }
}

$firmwareForbidden = @(
    'SERVER_TIME_URL',
    'NetworkTime_Sync',
    'HTTP_PostJsonBatch',
    'HTTP_PostReadyFileJson',
    'application/json',
    '\\"file_id\\"',
    '\\"batch_index\\"'
)

foreach ($pattern in $firmwareForbidden) {
    if ($firmware -match $pattern) {
        throw "Obsolete V1 JSON/time path remains: $pattern"
    }
}

$dashboardRequired = @(
    'createUploadParser',
    'telemetry-batch',
    'startsWith\("device_id,"\)',
    'state\.device',
    'device_id:\s*result\.batch\.device\.id',
    'accepted:\s*state\.rows\.length'
)

foreach ($pattern in $dashboardRequired) {
    if (($server + $parser) -notmatch $pattern) {
        throw "Missing dashboard CSV/device contract: $pattern"
    }
}

if ($server -match 'app\.get\("/api/time"' -or
    $server -match 'acceptedBatches' -or
    $server -match 'express\.json') {
    throw 'Dashboard still contains the retired network-time or JSON batch path.'
}

Write-Output 'V1 identified CSV upload contract: PASS'
