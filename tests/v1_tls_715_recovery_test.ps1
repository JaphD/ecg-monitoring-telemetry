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
$httpInit = $httpText.IndexOf('AT+HTTPINIT')
$sniConfig = $httpText.IndexOf('AT+CSSLCFG=\"enableSNI\",0,1')
$urlConfig = $httpText.IndexOf('AT+HTTPPARA=\"URL\"')
if (($httpInit -lt 0) -or ($sniConfig -lt 0) -or ($urlConfig -lt 0) -or
    -not (($httpInit -lt $sniConfig) -and ($sniConfig -lt $urlConfig))) {
    throw 'HTTPS transaction must enable SNI on SSL context 0 after HTTPINIT and before applying the URL.'
}
if ($httpText -notmatch 'Upload_CaptureFailure\("TLS_SNI"\)') {
    throw 'SNI configuration failure must be exposed as TLS_SNI.'
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
