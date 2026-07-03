$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    '#define MAX_HTTPDATA_BYTES\s+100000U',
    'volatile uint32_t upload_oversize_files',
    'static void QuarantineReadyFile\(const char \*path\)',
    'strstr\(bad_path,\s*"\.RDY"\)',
    'snprintf\(ext,\s*5U,\s*"\.BAD"\)',
    'f_rename\(path,\s*bad_path\)',
    'info\.fsize > MAX_HTTPDATA_BYTES',
    'upload_oversize_files\+\+',
    'QuarantineReadyFile\(path\)'
)

foreach ($pattern in $required) {
    if ($source -notmatch $pattern) {
        throw "Missing oversized upload quarantine contract: $pattern"
    }
}

Write-Output 'Oversized upload quarantine contract: PASS'
