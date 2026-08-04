$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    '#define DEVICE_ID_BUFFER_SIZE\s+31U',
    'volatile char device_id\[DEVICE_ID_BUFFER_SIZE\]',
    'static void DeviceIdentity_Init\(void\)',
    'HAL_GetUIDw0\(\)',
    'HAL_GetUIDw1\(\)',
    'HAL_GetUIDw2\(\)',
    'STM32-%08lX%08lX%08lX',
    'device_id,%s\\r\\n',
    'DeviceIdentity_Init\(\);',
    'Logger_NewPreambleSize\(\)',
    'MAX_HTTPDATA_BYTES\s+100000U'
)

foreach ($pattern in $required) {
    if ($source -notmatch $pattern) {
        throw "Missing device identity CSV contract: $pattern"
    }
}

$metadataWrite = $source.IndexOf('device_id,%s\r\n')
$headerWrite = $source.IndexOf('CSV_HEADER', $metadataWrite)
if (($metadataWrite -lt 0) -or ($headerWrite -le $metadataWrite)) {
    throw 'Device metadata must be written before the sample CSV header.'
}

$recoverCheck = [regex]::Match(
    $source,
    'static void Logger_RecoverQueue\(void\)([\s\S]*?)static uint8_t FindOldestReady'
).Groups[1].Value
$uploadCheck = [regex]::Match(
    $source,
    'static void Upload_OldestReady\(void\)([\s\S]*?)static void Background_Service'
).Groups[1].Value

foreach ($body in @($recoverCheck, $uploadCheck)) {
    if ($body -notmatch 'Logger_NewPreambleSize\(\)') {
        throw 'Both recovery and upload empty-file checks must understand the new preamble.'
    }
}

Write-Output 'STM32 device identity CSV contract: PASS'
