$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
  'JSON_BATCH_CAPACITY',
  'NetworkTime_Sync',
  'NetworkTime_TimestampForTick',
  'DeviceId_Init',
  'HTTP_PostJsonBatch',
  'HTTP_PostReadyFileJson',
  'AT+CCLK?',
  'application/json',
  'stm32l452-%08lX%08lX%08lX',
  "json_batch[batch_length++] = ']'",
  'device_id', 'readings'
)
foreach ($item in $required) {
  if ($source -notmatch [regex]::Escape($item)) {
    throw "Missing V1 JSON telemetry contract: $item"
  }
}

$upload = $source.IndexOf('static void Upload_OldestReady')
$jsonUpload = $source.IndexOf('HTTP_PostReadyFileJson')
if (($upload -lt 0) -or ($jsonUpload -lt 0) -or ($jsonUpload -gt $upload)) {
  throw 'JSON file upload must be defined before the upload queue controller.'
}

if ($source -notmatch 'NetworkTime_Sync\(\)') {
  throw 'The modem boot path must synchronize network time before V1 upload.'
}

Write-Output 'V1 JSON telemetry contract: PASS'
