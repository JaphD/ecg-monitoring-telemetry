$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$patterns = @(
  'modem_boot_stage',
  'modem_boot_failure',
  'Modem_TryAT',
  'Modem_ServiceDelay',
  'probing existing power',
  'PWRKEY power-on',
  'hardware reset recovery',
  'forced off/on recovery',
  'LTE registration',
  'PDP context ready'
)
foreach ($pattern in $patterns) {
  if ($source -notmatch $pattern) { throw "Missing modem boot behavior: $pattern" }
}

$bootStart = $source.IndexOf('static uint8_t Modem_Boot(void)')
$bootText = $source.Substring($bootStart, [Math]::Min(9000, $source.Length - $bootStart))
$probe = $bootText.IndexOf('Modem_TryAT')
$firstPulse = $bootText.IndexOf('GPIO_PIN_2, GPIO_PIN_SET')
if (($probe -lt 0) -or ($firstPulse -lt 0) -or ($probe -gt $firstPulse)) {
  throw 'The modem must be probed with AT before the first PWRKEY pulse.'
}

Write-Output 'Modem boot state machine contract: PASS'
