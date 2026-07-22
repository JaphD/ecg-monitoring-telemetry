$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    '#define JSON_BATCH_CAPACITY\s+32768U',
    'static uint8_t HTTP_PostReadyFileJson\(const char \*path\)',
    'JSON_BATCH_CAPACITY',
    'HTTP_PostJsonBatch\(json_batch, batch_length\)',
    'f_gets\(line, sizeof\(line\), &upload\)',
    'CSV_ROW_PARSE'
)

foreach ($pattern in $required) {
    if ($source -notmatch $pattern) {
        throw "Missing oversized upload quarantine contract: $pattern"
    }
}

Write-Output 'Oversized upload quarantine contract: PASS'
