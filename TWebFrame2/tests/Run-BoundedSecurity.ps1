param(
    [Parameter(Mandatory=$true)][string]$Url,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$Probe=(Join-Path $PSScriptRoot 'bin\x64\Release\PageIntegrationRegression.exe')
)
$ErrorActionPreference='Stop'
$outputPath=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path (Join-Path $outputPath 'scripts') | Out-Null
$previousObservation=$env:TWEBFRAME_OBSERVATION_ROOT
$env:TWEBFRAME_OBSERVATION_ROOT=$outputPath
try {
    # A new process per attempt; the in-process ten-second deadline is absolute.
    # The watchdog covers synchronous native/network calls and stalled cleanup.
    $watch=[Diagnostics.Stopwatch]::StartNew()
    $probeProcess=Start-Process -FilePath $Probe -ArgumentList @(
        ('"'+$Url+'"'),'10',('"'+$outputPath+'"'),'--native','--security-budget'
    ) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $outputPath 'run.log') -RedirectStandardError (Join-Path $outputPath 'run.err')
    $watchdog=$false
    if(!$probeProcess.WaitForExit(11000)) {
        $watchdog=true
        Stop-Process -Id $probeProcess.Id
        $probeProcess.WaitForExit()
    }
    $watch.Stop()
    $log=Get-Content -LiteralPath (Join-Path $outputPath 'run.log') -Raw
    $complete=!$watchdog -and $probeProcess.ExitCode -eq 0 -and $log -match '(?m)^SECURITY_RESULT complete elapsed_ms=(\d+)'
    $result=[ordered]@{
        budgetMs=10000
        complete=$complete
        result=if($complete){'complete'}elseif($watchdog){'watchdog-timeout'}elseif($log -match 'SECURITY_RESULT timeout'){'timeout'}else{'load-or-process-error'}
        processMs=[Math]::Round($watch.Elapsed.TotalMilliseconds,1)
        exitCode=$probeProcess.ExitCode
        watchdog=$watchdog
        # Completion must be in the ten-second window, even if diagnostics take longer.
        completionMs=if($complete){[int]$Matches[1]}else{$null}
    }
    if($complete -and $result.completionMs -ge 10000){$result.complete=$false;$result.result='late-completion'}
    $json=$result | ConvertTo-Json
    [IO.File]::WriteAllText((Join-Path $outputPath 'budget-result.json'),$json,[Text.UTF8Encoding]::new($false))
    Write-Output $json
} finally {
    $env:TWEBFRAME_OBSERVATION_ROOT=$previousObservation
    if($probeProcess){$probeProcess.Dispose()}
}
