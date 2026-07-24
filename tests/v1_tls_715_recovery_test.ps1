$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
    'tls_handshake_failures',
    'tls_recovery_pending',
    'tls_recovery_cycles',
    'AT+CSSLCFG=\"sslversion\",0,3',
    'AT+CSSLCFG=\"enableSNI\",0,1',
    'AT+HTTPPARA=\"SSLCFG\",0',
    'current_http_status == 715U',
    'ModemPower_Disable("TLS 715 recovery")'
)

foreach ($item in $required) {
    if ($source -notmatch [regex]::Escape($item)) {
        throw "Missing TLS 715 recovery contract: $item"
    }
}

$uploadStart = $source.IndexOf('static void Upload_OldestReady(void)')
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
