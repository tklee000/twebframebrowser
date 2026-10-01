param(
    [string]$Probe = (Join-Path $PSScriptRoot 'bin\x64\Release\PageScriptProbe.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'artifacts\browser-context-turnstile-current'),
    [ValidateRange(10, 120)][int]$ObservationSeconds = 30
)

# Uses only Cloudflare's documented always-pass and always-fail public keys.
# The fixture has no production keys, accounts or challenge interaction.
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $Probe)) { throw 'Build PageScriptProbe.vcxproj in Release|x64 first.' }
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
    foreach ($case in @('pass', 'fail')) {
        $caseDirectory = Join-Path $OutputDirectory $case
        $log = Join-Path $OutputDirectory ($case + '.log')
        & $Probe ($baseUrl + '?case=' + $case) $ObservationSeconds $caseDirectory --native *> $log
        if ($LASTEXITCODE -ne 0) { throw ($case + ' probe failed; see ' + $log) }
        $text = Get-Content -LiteralPath (Join-Path $caseDirectory 'page-script-text.txt') -Raw
        $layout = Get-Content -LiteralPath (Join-Path $caseDirectory 'page-script-layout.json') -Raw | ConvertFrom-Json
        $transportLog = Get-Content -LiteralPath $log -Raw
        $callback = if ($case -eq 'pass') { $text.Contains('TEST_ONLY|pass|success|dummy=true') }
                    else { $text -match 'TEST_ONLY\|fail\|error\|code=\d+' }
        $event = if ($case -eq 'pass') { 'complete' } else { 'fail' }
        if (!$callback -or $layout.runtime.receivedMessages -notcontains $event -or
            $layout.runtime.pendingPromises -ne 0 -or $transportLog -notmatch '(?m)^RUNTIME_ERROR\s*$' -or
            $transportLog -notmatch '(?m)^EXECUTION_TIME_LIMIT 0\s*$') {
            throw ($case + ' integration did not produce the expected callback and message; see ' + $log)
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
