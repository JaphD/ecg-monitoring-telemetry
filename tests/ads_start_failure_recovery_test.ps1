$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

$requiredDiagnostics = @(
    'ads_start_hard_failures',
    'ads_recovery_cycles',
    'ads_last_failed_id',
    'ads_recovery_reason'
)
foreach ($pattern in $requiredDiagnostics) {
    if ($source -notmatch $pattern) { throw "Missing ADS recovery diagnostic: $pattern" }
}

$recordStart = $source.IndexOf('static void Run_RecordPhase(void)')
$uploadStart = $source.IndexOf('static void Run_UploadPhase(void)', $recordStart)
if (($recordStart -lt 0) -or ($uploadStart -lt 0)) { throw 'Could not isolate Run_RecordPhase' }
$recordText = $source.Substring($recordStart, $uploadStart - $recordStart)

if ($recordText -match 'ADS start failed: ID=.*Error_Handler\(\)') {
    throw 'ADS start failure must not enter Error_Handler.'
}
if ($recordText -notmatch 'Handle_ADSStartFailure\(\)') {
    throw 'Run_RecordPhase must delegate ADS start failure to recoverable handling.'
}

$handlerStart = $source.IndexOf('static void Handle_ADSStartFailure(void)')
$recordStart = $source.IndexOf('static void Run_RecordPhase(void)')
if (($handlerStart -lt 0) -or ($recordStart -lt 0)) { throw 'Missing ADS start failure handler' }
$handlerText = $source.Substring($handlerStart, $recordStart - $handlerStart)

$handlerPatterns = @(
    'ads_start_hard_failures\+\+',
    'ads_last_failed_id = ads_id_value',
    'ADS_StopAcquisition\(\)',
    'HAL_SPI_DeInit\(&hspi1\)',
    'HAL_SPI_Init\(&hspi1\)',
    'system_phase = 60U',
    'ADS recovery'
)
foreach ($pattern in $handlerPatterns) {
    if ($handlerText -notmatch $pattern) { throw "ADS recovery handler missing: $pattern" }
}

Write-Output 'ADS start failure recovery contract: PASS'
