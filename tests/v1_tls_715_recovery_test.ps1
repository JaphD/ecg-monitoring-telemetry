$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    'tls_handshake_failures',
    'tls_recovery_pending',
    'tls_recovery_cycles',
    'current_http_status == 715U',
    'ModemPower_Disable("TLS 715 recovery")'
)

foreach ($item in $required) {
    if ($source -notmatch [regex]::Escape($item)) {
        throw "Missing TLS 715 recovery contract: $item"
    }
}

$httpStart = $source.IndexOf('static uint8_t HTTP_PostFile(')
$uploadStart = $source.IndexOf('static void Upload_OldestReady(void)')
if (($httpStart -lt 0) -or ($uploadStart -lt 0) -or ($uploadStart -le $httpStart)) {
    throw 'HTTP post or upload function not found.'
}

$httpText = $source.Substring($httpStart, $uploadStart - $httpStart)
$unsupportedOverrides = @(
    'AT+CSSLCFG=\"sslversion\"',
    'AT+CSSLCFG=\"enableSNI\"',
    'AT+HTTPPARA=\"SSLCFG\"'
)

foreach ($item in $unsupportedOverrides) {
    if ($httpText -match [regex]::Escape($item)) {
        throw "HTTPS transaction must use the validated modem-default TLS context: $item"
    }
}

$drainStart = $source.IndexOf('static void Drain_UploadQueueBeforeNextRecord(void)')
if (($uploadStart -lt 0) -or ($drainStart -lt 0)) {
    throw 'Upload or queue-drain function not found.'
}

$uploadText = $source.Substring($uploadStart, $drainStart - $uploadStart)
if ($uploadText -notmatch 'if \(!uploaded\)[\s\S]*if \(saw_tls_715\) tls_recovery_pending = 1U') {
    throw 'TLS recovery must be requested only after the complete upload round fails.'
}

$drainText = $source.Substring($drainStart, 3000)
if ($drainText -notmatch 'if \(tls_recovery_pending != 0U\)[\s\S]*ModemPower_Disable\("TLS 715 recovery"\)[\s\S]*tls_recovery_pending = 0U') {
    throw 'The queue retry path must perform and clear one pending TLS rail recovery.'
}
if ($drainText -match 'ModemPower_Disable\("upload retry"\)') {
    throw 'Ordinary queue retries must continue retaining modem power.'
}

Write-Output 'V1 TLS 715 recovery contract: PASS'
