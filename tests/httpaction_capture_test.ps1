$ErrorActionPreference = 'Stop'

$sourcePath = Join-Path $PSScriptRoot '..\Core\Src\main.c'
$source = Get-Content -Raw $sourcePath

if ($source -notmatch 'Modem_Command\("AT\+HTTPACTION=1[\s\S]{0,120}"\+HTTPACTION:"[\s\S]{0,80}1U\)') {
    throw 'HTTPACTION capture must wait for a complete line, not only the prefix.'
}

$partial = "`r`nOK`r`n`r`n+HTTPACTION:"
$complete = "`r`nOK`r`n`r`n+HTTPACTION: 1,200,11`r`n"

if ($partial -match '\+HTTPACTION:[^\r\n]*\r?\n') {
    throw 'Partial HTTPACTION prefix was incorrectly treated as a complete line.'
}

if ($complete -notmatch '\+HTTPACTION:[^\r\n]*\r?\n') {
    throw 'Complete HTTPACTION line was not recognized.'
}

Write-Output 'HTTPACTION complete-line capture contract: PASS'
