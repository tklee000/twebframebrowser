param(
    [string]$Probe = (Join-Path $PSScriptRoot 'bin\x64\Release\PageIntegrationRegression.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'artifacts\browser-context-turnstile-current'),
    [ValidateRange(15, 120)][int]$ObservationSeconds = 30
)

# Uses only Cloudflare's documented pass, fail and interactive public test keys.
# Pointer interaction is confined to the local test-key fixture.
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $Probe)) { throw 'Build PageIntegrationRegression.vcxproj in Release|x64 first.' }
$Probe = (Resolve-Path -LiteralPath $Probe).Path
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$nodePath = (Get-Command node -CommandType Application | Select-Object -First 1).Source
$serverScript = Join-Path $PSScriptRoot 'turnstile-test-server.cjs'
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()
$server = $null
try {
    $launch = @{
        FilePath = $nodePath
        ArgumentList = @(('"' + $serverScript + '"'), $port)
        WindowStyle = 'Hidden'
        PassThru = $true
        RedirectStandardOutput = (Join-Path $OutputDirectory 'server.log')
        RedirectStandardError = (Join-Path $OutputDirectory 'server.err')
    }
    $server = Start-Process @launch
    $baseUrl = 'http://127.0.0.1:' + $port + '/turnstile-integration-test.html'
    $ready = $false
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        if ($server.HasExited) { throw 'The test fixture server exited.' }
        try {
            $response = Invoke-WebRequest -Uri $baseUrl -TimeoutSec 2 -UseBasicParsing
            if ($response.StatusCode -eq 200) { $ready = $true; break }
        } catch { Start-Sleep -Milliseconds 100 }
    }
    if (!$ready) { throw 'The test fixture server did not become ready.' }
    foreach ($case in @('pass', 'fail', 'interactive')) {
        $caseDirectory = Join-Path $OutputDirectory $case
        $log = Join-Path $OutputDirectory ($case + '.log')
        $probeOptions = @('--native')
        # DIP coordinates of the checkbox in this fixed-size diagnostic fixture.
        if ($case -eq 'interactive') { $probeOptions += '--click=48,150' }
        if ($case -eq 'fail') { $probeOptions += '--click=94,158' }
        & $Probe ($baseUrl + '?case=' + $case) $ObservationSeconds $caseDirectory @probeOptions *> $log
        if ($LASTEXITCODE -ne 0) { throw ($case + ' probe failed; see ' + $log) }
        $text = Get-Content -LiteralPath (Join-Path $caseDirectory 'page-script-text.txt') -Raw
        $layout = Get-Content -LiteralPath (Join-Path $caseDirectory 'page-script-layout.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        $transportLog = Get-Content -LiteralPath $log -Raw
        $callback = if ($case -eq 'fail') { $text -match 'TEST_ONLY\|fail\|error\|code=\d+' }
                    else { $text.Contains('TEST_ONLY|' + $case + '|success|dummy=true') }
        $event = if ($case -eq 'fail') { 'fail' } else { 'complete' }
        if (!$callback -or $layout.runtime.receivedMessages -notcontains $event -or
            $layout.runtime.pendingPromises -ne 0 -or $transportLog -notmatch '(?m)^RUNTIME_ERROR\s*$' -or
            $transportLog -notmatch '(?m)^EXECUTION_TIME_LIMIT 0\s*$') {
            throw ($case + ' integration did not produce the expected callback and message; see ' + $log)
        }
        if ($case -eq 'interactive') {
            $before = Get-Content -LiteralPath (Join-Path $caseDirectory 'page-script-before-click.json') -Raw | ConvertFrom-Json
            if ($before.runtime.receivedMessages -notcontains 'interactiveBegin' -or
                $before.runtime.receivedMessages -contains 'complete' -or
                $layout.runtime.receivedMessages -notcontains 'interactiveEnd' -or
                $transportLog -notmatch '(?m)^POINTER_CLICK 48,150 error=\s*$') {
                throw ('Interactive test-key completion must follow the native pointer click; see ' + $log)
            }
        }
        if ($case -eq 'fail') {
            $details = $layout.frames | Where-Object { $_.document.id -contains 'fr-title' } | Select-Object -First 1
            $title = $details.document | Where-Object id -eq 'fr-title' | Select-Object -First 1
            $runs = @($details.document | Where-Object { $_.tag -eq '#text' -and $_.text.Trim() -and $_.height -gt 0 })
            $frame = $layout.document | Where-Object { $_.tag -eq 'iframe' -and $_.id -like '*-fr' } | Select-Object -First 1
            if (!$details -or !$title -or !$frame -or $runs.Count -lt 5 -or
                $layout.runtime.receivedMessages -notcontains 'feedbackInit' -or
                $layout.runtime.receivedMessages -notcontains 'feedbackOpen' -or
                $transportLog -notmatch '(?m)^POINTER_CLICK 94,158 error=\s*$') {
                throw ('Failure details did not initialize and create their visible text; see ' + $log)
            }
            # Check glyph pixels, since populated DOM text alone can still paint blank.
            Add-Type -AssemblyName System.Drawing
            $bitmap = [System.Drawing.Bitmap]::new((Join-Path $caseDirectory 'page-script-render.bmp'))
            try {
                $ink = 0
                for ($y = [int]($frame.y + $title.y); $y -lt [int]($frame.y + $title.y + $title.height); $y++) {
                    for ($x = [int]($frame.x + $title.x); $x -lt [int]($frame.x + $title.x + $title.width); $x++) {
                        $pixel = $bitmap.GetPixel($x, $y)
                        if ($pixel.R -lt 180 -and $pixel.G -lt 180 -and $pixel.B -lt 180) { $ink++ }
                    }
                }
                if ($ink -lt 50) { throw ('Failure details title painted blank; see ' + $log) }
            } finally { $bitmap.Dispose() }
            Write-Output ('PASS: native Turnstile failure details initialize and paint title glyphs (' + $ink + ' pixels)')
        }
        Write-Output ('PASS: native Turnstile test-key ' + $case + ' callback and iframe message')
    }
} finally {
    if ($server) {
        $server.Refresh()
        if (!$server.HasExited) {
            $process = Get-CimInstance Win32_Process -Filter ('ProcessId=' + $server.Id)
            if ($process -and $process.CommandLine.Contains($serverScript)) { Stop-Process -Id $server.Id }
        }
        $server.Dispose()
    }
}
