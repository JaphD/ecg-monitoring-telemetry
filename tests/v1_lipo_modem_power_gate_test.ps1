$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$required = @(
  'MODEM_RAIL_SETTLE_MS       100U',
  'ModemPower_Enable', 'ModemPower_Disable', 'ModemPower_BootForUpload',
  'modem_power_requested', 'modem_power_state', 'modem_power_stage',
  'modem_power_enables', 'modem_power_disables', 'modem_power_cycles',
  'modem_power_last_on_reason', 'modem_power_last_off_reason'
)
foreach ($item in $required) {
  if ($source -notmatch [regex]::Escape($item)) {
    throw "Missing V1 modem power contract: $item"
  }
}

$enable = $source.IndexOf('static void ModemPower_Enable')
$boot = $source.IndexOf('static uint8_t ModemPower_BootForUpload')
$upload = $source.IndexOf('static void Run_UploadPhase')
if (($enable -lt 0) -or ($boot -lt 0) -or ($upload -lt 0) -or ($boot -gt $upload)) {
  throw 'Power controller must be defined before the upload phase.'
}

$retryStart = $source.IndexOf('static void Drain_UploadQueueBeforeNextRecord')
$retryText = $source.Substring($retryStart, 2600)
if ($retryText -notmatch 'ModemPower_Disable\("upload retry"\)') {
  throw 'Retry idle must force the modem rail off.'
}
if ($retryText -notmatch 'UPLOAD_RETRY_IDLE_MS') {
  throw 'Retry cooldown must remain intact.'
}
if ($source -notmatch 'ModemPower_Disable\("upload complete"\)') {
  throw 'Successful upload must power off the modem rail.'
}

Write-Output 'V1 modem power-gate contract: PASS'
