$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$patterns = @(
  '#define MODEM_RX_DRAIN_MAX_BYTES\s+64U',
  'modem_boot_stage',
  'modem_boot_failure',
  'modem_rx_drain_bytes',
  'modem_rx_drain_limit_hits',
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

$rxStart = $source.IndexOf('static HAL_StatusTypeDef Modem_StartRx(void)')
$rxEnd = $source.IndexOf('void HAL_UART_RxCpltCallback', $rxStart)
if (($rxStart -lt 0) -or ($rxEnd -lt 0)) {
  throw 'Could not isolate Modem_StartRx.'
}
$rx = $source.Substring($rxStart, $rxEnd - $rxStart)
if ($rx -match 'while\s*\(\s*HAL_UART_Receive') {
  throw 'Modem_StartRx must not use an unbounded receive-drain loop.'
}
if (($rx -notmatch 'drained < MODEM_RX_DRAIN_MAX_BYTES') -or
    ($rx -notmatch 'modem_rx_drain_limit_hits\+\+')) {
  throw 'Modem RX drain must stop at a debugger-visible byte limit.'
}

$bootStart = $source.IndexOf('static uint8_t Modem_Boot(void)')
$bootText = $source.Substring($bootStart, [Math]::Min(9000, $source.Length - $bootStart))
$probe = $bootText.IndexOf('Modem_TryAT')
$firstPulse = $bootText.IndexOf('GPIO_PIN_2, GPIO_PIN_SET')
if (($probe -lt 0) -or ($firstPulse -lt 0) -or ($probe -gt $firstPulse)) {
  throw 'The modem must be probed with AT before the first PWRKEY pulse.'
}

Write-Output 'Modem boot state machine contract: PASS'
