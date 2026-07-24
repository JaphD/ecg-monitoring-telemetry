$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

if ($source -match '"timestamp":%llu') {
    throw 'JSON timestamps must not depend on unsupported newlib-nano %llu formatting.'
}

$required = @(
    'MODEM_HTTP_TX_CHUNK_SIZE\s+512U',
    'MODEM_HTTP_TX_PACING_MS',
    'static uint8_t UInt64_ToDecimal\(uint64_t value, char \*buffer, size_t capacity\)',
    'UInt64_ToDecimal\(NetworkTime_TimestampForTick\(\(uint32_t\)tick\)',
    '\\"timestamp\\":%s',
    'while \(upload_bytes_sent < payload_size\)',
    'if \(chunk_size > MODEM_HTTP_TX_CHUNK_SIZE\)',
    'HAL_UART_Transmit\(&huart1,\s*\(uint8_t \*\)\(payload \+ upload_bytes_sent\)',
    'upload_uart_chunks\+\+',
    'upload_uart_last_chunk_size = chunk_size',
    'upload_uart_chunk_failures\+\+',
    'Background_Service\(\)'
)

foreach ($pattern in $required) {
    if ($source -notmatch $pattern) {
        throw "Missing JSON/UART integrity contract: $pattern"
    }
}

Write-Output 'JSON timestamp and UART chunking regression contract: PASS'
