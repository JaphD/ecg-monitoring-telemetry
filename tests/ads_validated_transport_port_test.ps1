$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')

function Get-FunctionBody([string]$signature, [string]$nextSignature) {
    $start = $source.IndexOf($signature)
    $end = $source.IndexOf($nextSignature, $start + $signature.Length)
    if (($start -lt 0) -or ($end -lt 0)) {
        throw "Could not isolate function: $signature"
    }
    return $source.Substring($start, $end - $start)
}

$requiredDefinitions = @(
    '#define ADS_SPI_GUARD_MS\s+2U',
    '#define ADS_SETTLING_FRAMES\s+4U',
    '#define ADS_COMMAND_MODE_RETRIES\s+2U',
    '#define ADS_REGISTER_VERIFY_RETRIES\s+3U',
    'static HAL_StatusTypeDef ADS_EnterCommandMode\(',
    'static HAL_StatusTypeDef ADS_WriteAndVerify\(',
    'static HAL_StatusTypeDef ADS_StartContinuous\(',
    'hspi1\.Init\.NSSPMode = SPI_NSS_PULSE_DISABLE;'
)
foreach ($pattern in $requiredDefinitions) {
    if ($source -notmatch $pattern) {
        throw "Missing validated ADS transport contract: $pattern"
    }
}

$write = Get-FunctionBody `
    'static HAL_StatusTypeDef ADS_WriteRegister' `
    'static HAL_StatusTypeDef ADS_ReadRegister'
if ($write -notmatch 'for\s*\([^)]*sizeof\(tx\)') {
    throw 'WREG must transmit command, count, and value as guarded individual bytes.'
}
if (($write | Select-String -AllMatches 'HAL_Delay\(ADS_SPI_GUARD_MS\)').Matches.Count -lt 3) {
    throw 'WREG is missing per-byte SPI decode guards.'
}

$read = Get-FunctionBody `
    'static HAL_StatusTypeDef ADS_ReadRegister' `
    'static HAL_StatusTypeDef ADS_ReadFrame'
if ($read -match 'HAL_SPI_Transmit\(&hspi1,\s*header,\s*2U') {
    throw 'RREG must not transmit its two-byte header as one uninterrupted burst.'
}
if (($read | Select-String -AllMatches 'HAL_Delay\(ADS_SPI_GUARD_MS\)').Matches.Count -lt 4) {
    throw 'RREG is missing command/count/data SPI decode guards.'
}

$commandMode = Get-FunctionBody `
    'static HAL_StatusTypeDef ADS_EnterCommandMode' `
    'static HAL_StatusTypeDef ADS_WriteAndVerify'
$sdatac = $commandMode.IndexOf('ADS_CMD_SDATAC')
$firstProbe = $commandMode.IndexOf('ADS_CommandModeProbe', $sdatac)
$stop = $commandMode.IndexOf('ADS_CMD_STOP', $firstProbe)
$secondProbe = $commandMode.IndexOf('ADS_CommandModeProbe', $stop)
if (($sdatac -lt 0) -or ($firstProbe -lt 0) -or ($stop -lt 0) -or ($secondProbe -lt 0)) {
    throw 'Command-mode entry must perform SDATAC, ID probe, STOP, and a second ID probe.'
}

$setup = Get-FunctionBody `
    'static HAL_StatusTypeDef ADS_ConfigureAndStartAttempt' `
    'static HAL_StatusTypeDef ADS_StartAcquisition'
$requiredVerifiedWrites = @(
    'ADS_WriteAndVerify\(ADS_REG_CONFIG1,\s*ADS_CONFIG1_250_SPS\)',
    'ADS_WriteConfig2Lenient\(ADS_CONFIG2_EXTERNAL\)',
    'ADS_WriteAndVerify\(ADS_REG_LOFF,\s*ADS_LOFF_MAIN\)',
    'ADS_WriteAndVerify\(ADS_REG_CH1SET,\s*ADS_CH1SET_INTERNAL_SHORT\)',
    'ADS_WriteAndVerify\(ADS_REG_CH2SET,\s*ADS_CH2SET_NORMAL\)',
    'ADS_WriteAndVerify\(ADS_REG_RLD_SENS,\s*ADS_RLD_SENS_MAIN\)',
    'ADS_WriteAndVerify\(ADS_REG_LOFF_SENS,\s*ADS_LOFF_SENS_MAIN\)',
    'ADS_WriteAndVerify\(ADS_REG_RESP1,\s*ADS_RESP1_MAIN\)',
    'ADS_WriteAndVerify\(ADS_REG_RESP2,\s*ADS_RESP2_MAIN\)',
    'ADS_StartContinuous\(\)'
)
foreach ($pattern in $requiredVerifiedWrites) {
    if ($setup -notmatch $pattern) {
        throw "Production ADS setup is not using the validated operation: $pattern"
    }
}

$startContinuous = Get-FunctionBody `
    'static HAL_StatusTypeDef ADS_StartContinuous' `
    'static HAL_StatusTypeDef ADS_ConfigureAndStartAttempt'
if ($startContinuous -notmatch 'for\s*\([^)]*ADS_SETTLING_FRAMES') {
    throw 'Continuous startup must discard the validated settling-frame count.'
}

Write-Output 'Validated ADS1292R transport port contract: PASS'
