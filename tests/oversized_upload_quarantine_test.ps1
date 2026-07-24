$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    '#define MAX_HTTPDATA_BYTES\s+100000U',
    'static void QuarantineReadyFile\(const char \*path\)',
    'info\.fsize > MAX_HTTPDATA_BYTES',
    'QuarantineReadyFile\(path\)',
    'upload_oversize_files\+\+',
    'if \(HTTP_PostFile\(path\)\)'
)

foreach ($pattern in $required) {
    if ($source -notmatch $pattern) {
        throw "Missing oversized upload quarantine contract: $pattern"
    }
}

Write-Output 'Oversized upload quarantine contract: PASS'
