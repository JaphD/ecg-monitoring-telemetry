$ErrorActionPreference = 'Stop'
$main = Get-Content -Raw (Join-Path $PSScriptRoot '..\Core\Src\main.c')
$disk = Get-Content -Raw (Join-Path $PSScriptRoot '..\FATFS\Target\sd_diskio.c')

$mainPatterns = @(
    'sd_upload_read_active',
    'upload_stage',
    'if (!sd_upload_read_active) Logger_Drain(32U)',
    'upload_stage = 30U',
    'upload_stage = 40U',
    'upload_stage = 70U',
    'upload_stage = 100U'
)
foreach ($pattern in $mainPatterns) {
    if ($main.IndexOf($pattern) -lt 0) {
        throw "Missing isolated SD upload phase contract: $pattern"
    }
}

$postStart = $main.IndexOf('static uint8_t HTTP_PostFile(const char *path)')
$postEnd = $main.IndexOf('static void Upload_OldestReady(void)', $postStart)
$post = $main.Substring($postStart, $postEnd - $postStart)
$close = $post.IndexOf('f_close(&upload)')
$readActive = $post.IndexOf('sd_upload_read_active = 1U')
$fileRead = $post.IndexOf('f_read(&upload, chunk, sizeof(chunk), &bytes_read)')
if (($close -lt 0) -or ($readActive -lt 0) -or ($fileRead -lt 0) -or
    ($readActive -gt $fileRead) -or ($fileRead -gt $close)) {
    throw 'CSV upload must isolate SD reads until the ready file is closed.'
}

if ($disk -notmatch 'SD_WaitForTransfer') {
    throw 'The SD polling driver needs a bounded transfer-state helper.'
}
if ($disk -notmatch 'HAL_GetTick\(\)\s*-\s*start') {
    throw 'The SD transfer-state wait must use a HAL tick timeout.'
}

Write-Output 'SD upload phase isolation and bounded driver wait: PASS'
