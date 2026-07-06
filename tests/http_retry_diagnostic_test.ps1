$ErrorActionPreference = 'Stop'

$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$requiredPatterns = @(
    '#define HTTP_MAX_ATTEMPTS\s+3U',
    'last_upload_attempts',
    'attempt\s*=\s*1U;\s*attempt\s*<=\s*HTTP_MAX_ATTEMPTS',
    'AT\+CEREG\?',
    'AT\+CGATT=1',
    'AT\+CGACT=1,1',
    'AT\+CGPADDR=1',
    'HTTP_RETRY_BACKOFF_MS\s+5000U'
)

foreach ($pattern in $requiredPatterns) {
    if ($source -notmatch $pattern) {
        throw "Missing retry diagnostic contract: $pattern"
    }
}

Write-Output 'Production HTTP retry/network readiness contract: PASS'
