param(
    [string]$Url = 'https://www.ppomppu.co.kr/zboard/login.php',
    [ValidateRange(1,20)][int]$Visits = 10,
    [ValidateRange(10,120)][int]$ObservationSeconds = 30,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('artifacts\cloudflare-recapture-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
)
$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$probe = Join-Path $PSScriptRoot 'bin\x64\Release\WebView2TraceProbe.exe'
if (!(Test-Path -LiteralPath $probe)) { throw ('Build the capture probe first: ' + $probe) }
if ((Test-Path -LiteralPath $OutputDirectory) -and (Get-ChildItem -LiteralPath $OutputDirectory | Select-Object -First 1)) {
    throw 'Choose a new output directory to preserve earlier visits.'
}
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$batch = [ordered]@{
    url = $Url
    requestedVisits = $Visits
    observationSeconds = $ObservationSeconds
    startedUtc = [DateTime]::UtcNow.ToString('o')
    probeSha256 = (Get-FileHash -LiteralPath $probe -Algorithm SHA256).Hash.ToLowerInvariant()
    freshProfilePerVisit = $true
    attempts = @()
}
$batchPath = Join-Path $OutputDirectory 'capture-session.json'
$batch | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $batchPath -Encoding UTF8
Write-Output ('Output: ' + $OutputDirectory)
foreach ($visit in 1..$Visits) {
    $label = 'visit-' + $visit.ToString('00')
    $folder = Join-Path $OutputDirectory ('webview2\' + $label)
    $log = Join-Path $OutputDirectory ('webview2-' + $label + '.log')
    $attempt = [ordered]@{ visit = $label; startedUtc = [DateTime]::UtcNow.ToString('o'); exitCode = $null; error = $null }
    Write-Output ('Starting ' + $label + ' (' + $visit + '/' + $Visits + ')')
    try {
        & $probe $Url $ObservationSeconds $folder *> $log
        $attempt.exitCode = $LASTEXITCODE
        if ($LASTEXITCODE -ne 0) { throw ('Probe exit code ' + $LASTEXITCODE) }
        $summary = Get-Content -LiteralPath (Join-Path $folder 'summary.json') -Raw | ConvertFrom-Json
        $result = Get-Content -LiteralPath (Join-Path $folder 'result.json') -Raw | ConvertFrom-Json
        $savedSources = @(Get-ChildItem -LiteralPath (Join-Path $folder 'scripts') -File | Where-Object { $_.Extension -in @('.js','.wasm') }).Count
        Write-Output ($label + ': scripts=' + $summary.scripts + '/' + $savedSources + ' responses=' + $summary.responses + ' errors=' + $summary.cdpErrors + ' pending=' + $summary.pending + ' tokenPresent=' + $result.tokenPresent)
    } catch {
        $attempt.error = $_.Exception.Message
        Write-Output ($label + ': failed; raw output preserved. ' + $attempt.error)
    }
    $attempt.endedUtc = [DateTime]::UtcNow.ToString('o')
    $batch.attempts += $attempt
    $batch | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $batchPath -Encoding UTF8
}
$batch.endedUtc = [DateTime]::UtcNow.ToString('o')
$batch | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $batchPath -Encoding UTF8
& node (Join-Path $PSScriptRoot 'Summarize-WebView2SecurityCaptures.cjs') $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Capture validation failed; all original files are preserved.' }
