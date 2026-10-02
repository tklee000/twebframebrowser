param(
    [string]$Url = 'https://www.ppomppu.co.kr/zboard/login.php',
    [ValidateRange(2,10)][int]$Visits = 3,
    [ValidateRange(10,120)][int]$WebViewSeconds = 30,
    [ValidateRange(30,300)][int]$TWebSeconds = 90,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('artifacts\cloudflare-stages-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
)
$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $OutputDirectory) -and (Get-ChildItem -LiteralPath $OutputDirectory | Select-Object -First 1)) {
    throw 'Choose a new output directory to preserve earlier visits.'
}
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$web = Join-Path $PSScriptRoot 'bin\x64\Release\WebView2TraceProbe.exe'
$tweb = Join-Path $PSScriptRoot 'bin\x64\Release\PageScriptProbe.exe'
foreach ($probe in @($web,$tweb)) { if (!(Test-Path -LiteralPath $probe)) { throw ('Build the diagnostic probe first: ' + $probe) } }
$names = @('TWEBFRAME_TRACE_FUNCTIONS','TWEBFRAME_TRACE_ERRORS','TWEBFRAME_TRACE_MISSING','TWEBFRAME_PROFILE_EXECUTION','TWEBFRAME_SCRIPT_ARCHIVE')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process'); [Environment]::SetEnvironmentVariable($name,$null,'Process') }
try {
    foreach ($engine in @('webview2','twebframe2')) {
        foreach ($visit in 1..$Visits) {
            $label = 'visit-' + $visit.ToString('00')
            $folder = Join-Path $OutputDirectory ($engine + '\' + $label)
            $log = Join-Path $OutputDirectory ($engine + '-' + $label + '.log')
            Write-Output ($engine + ' ' + $label)
            if ($engine -eq 'webview2') { & $web $Url $WebViewSeconds $folder *> $log }
            else { & $tweb $Url $TWebSeconds $folder --native --archive *> $log }
            if ($LASTEXITCODE -ne 0) { throw ('Probe failed; preserved output: ' + $log) }
        }
    }
    & node (Join-Path $PSScriptRoot 'Summarize-StageCaptures.cjs') $OutputDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Capture summary failed; raw visits are preserved.' }
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
}
