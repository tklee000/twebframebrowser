param(
    [Parameter(Mandatory=$true)][ValidateSet('webview2','twebframe2')][string]$Engine,
    [string]$CorpusPath=(Join-Path $PSScriptRoot '..\rendering-stress'),
    [string]$OutputDirectory='',
    [string[]]$CaseId=@(),
    [ValidateSet(96,144)][int[]]$Dpi=@(96,144),
    [int]$Width=800,[int]$Height=600,
    [ValidateRange(1,300)][int]$TimeoutSeconds=30,
    [ValidateRange(1,6)][int]$Workers=1,
    [switch]$Software,[switch]$SkipBuild,[switch]$ParallelDpi,[switch]$FullDiagnostics,
    [string]$BuildDirectory=(Join-Path $PSScriptRoot '.work\build')
)
$ErrorActionPreference='Stop'
if($Engine -ne 'twebframe2' -and $Workers -ne 1){throw 'Multiple workers are supported only for native captures.'}
if(!$Dpi.Count -or @($Dpi | Sort-Object -Unique).Count -ne $Dpi.Count){throw 'Select at least one DPI, without duplicates.'}
if($Width -lt 1 -or $Height -lt 1 -or $Width -gt 8192 -or $Height -gt 8192){throw 'CSS viewport dimensions must be between 1 and 8192.'}
foreach($value in $Dpi){
    $pixelWidth=$Width*$value/96.0;$pixelHeight=$Height*$value/96.0
    if($pixelWidth -ne [Math]::Round($pixelWidth) -or $pixelHeight -ne [Math]::Round($pixelHeight)){
        throw 'The CSS viewport must map exactly to whole device pixels; use even width/height at 150%.'
    }
    if($pixelWidth*$pixelHeight -gt 64000000){throw 'The physical viewport exceeds the shared 64-million-pixel capture limit.'}
}
$corpusFull=(Resolve-Path -LiteralPath $CorpusPath).Path
$exe=Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'bin\RenderingCapture.exe'
if (!$SkipBuild) { & (Join-Path $PSScriptRoot 'Build-Capture.ps1') -BuildDirectory $BuildDirectory }
if (!(Test-Path -LiteralPath $exe)) { throw 'Build the capture host first.' }
if (!$OutputDirectory) { $OutputDirectory=Join-Path $PSScriptRoot ('.work\runs\'+$Engine+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$outputFull=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputFull) { throw "Use a new output directory: $outputFull" }
$manifest=Get-Content -LiteralPath (Join-Path $corpusFull 'manifest.json') -Raw | ConvertFrom-Json
$cases=@($manifest.cases)
if ($CaseId.Count) {
    foreach($requested in $CaseId){if($requested -notin $cases.id){throw "Unknown case: $requested"}}
    $cases=@($cases | Where-Object {$_.id -in $CaseId})
}
$caseList=Join-Path $outputFull 'cases.txt'
$paths=@()
foreach($case in $cases){
    $path=[IO.Path]::GetFullPath((Join-Path $corpusFull $case.path))
    foreach($pair in @(@('index.html','htmlSha256'),@('style.css','cssSha256'))){
        $actual=(Get-FileHash -LiteralPath (Join-Path $path $pair[0]) -Algorithm SHA256).Hash.ToLowerInvariant()
        if($actual -ne $case.($pair[1])){throw "Input hash changed: $($case.id)/$($pair[0])"}
    }
    $paths += $path
}
[IO.Directory]::CreateDirectory($outputFull) | Out-Null
[IO.File]::WriteAllLines($caseList,$paths,[Text.UTF8Encoding]::new($false))
$runMetadata=@{schemaVersion=1;engine=$Engine;cases=$cases.Count;dpi=$Dpi;width=$Width;height=$Height;workersPerDpi=$Workers;softwareRequested=[bool]$Software;captureComplete=$false;timeoutSeconds=$TimeoutSeconds;manifestSha256=(Get-FileHash -LiteralPath (Join-Path $corpusFull 'manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant();executableSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()}
$runMetadata.fullNativeDiagnostics=[bool]$FullDiagnostics
$runMetadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputFull 'run.json') -Encoding UTF8
$runErrors=0
function Wait-DpiCapture($worker){
    $process=$worker.Process;$dpiOutput=$worker.Output;$scaleDpi=$worker.Dpi
    $previous='';$lastUpdate=[DateTime]::MinValue
    while(!$process.WaitForExit(1000)){
        $progressPath=Join-Path $dpiOutput 'progress.txt'
        if(Test-Path -LiteralPath $progressPath){$progress=Get-Content -LiteralPath $progressPath -Raw;if($progress -ne $previous -and ((Get-Date)-$lastUpdate).TotalSeconds -ge 30){Write-Host "$Engine $scaleDpi DPI: $($progress.Trim())";$previous=$progress;$lastUpdate=Get-Date}}
    }
    $summaryPath=Join-Path $dpiOutput 'summary.json'
    if(Test-Path -LiteralPath $summaryPath){Write-Host (Get-Content -LiteralPath $summaryPath -Raw)}
    else {Write-Host (Get-Content -LiteralPath (Join-Path $outputFull ($worker.LogPrefix+'-stderr.log')) -Raw);throw 'Capture process exited without summary.'}
    return [int]($process.ExitCode -ne 0)
}
$pending=@()
foreach($scaleDpi in $Dpi){
    $dpiPending=@()
    for($index=0;$index -lt [Math]::Min($Workers,$paths.Count);$index++){
        $workerList=$caseList;$prefix=$scaleDpi.ToString()
        $dpiOutput=Join-Path $outputFull ($Width.ToString()+'x'+$Height+'-'+$scaleDpi+'dpi')
        if($Workers -gt 1){
            $prefix+='-worker'+$index
            $dpiOutput=Join-Path $outputFull ('.workers\'+$prefix)
            $workerList=Join-Path $outputFull ($prefix+'-cases.txt')
            $workerPaths=@();for($j=$index;$j -lt $paths.Count;$j+=$Workers){$workerPaths+=$paths[$j]}
            [IO.File]::WriteAllLines($workerList,$workerPaths,[Text.UTF8Encoding]::new($false))
        }
        $arguments=@('--engine',$Engine,'--cases',('"'+$workerList+'"'),'--out',('"'+$dpiOutput+'"'),'--width',$Width,'--height',$Height,'--dpi',$scaleDpi,'--timeout-ms',($TimeoutSeconds*1000),'--measure',('"'+(Join-Path $PSScriptRoot 'measure.js')+'"'),'--software',$(if($Software){'true'}else{'false'}))
        $arguments+=@('--full-diagnostics',$(if($FullDiagnostics){'true'}else{'false'}))
        $process=Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $outputFull ($prefix+'-stdout.log')) -RedirectStandardError (Join-Path $outputFull ($prefix+'-stderr.log'))
        $worker=[PSCustomObject]@{Process=$process;Dpi=$scaleDpi;Output=$dpiOutput;LogPrefix=$prefix}
        $dpiPending+=$worker
    }
    if($ParallelDpi){$pending+=$dpiPending}else{foreach($worker in $dpiPending){$runErrors+=Wait-DpiCapture $worker}}
}
foreach($worker in $pending){$runErrors+=Wait-DpiCapture $worker}
if($Workers -gt 1){
    # Merge disjoint whole-document captures without modifying PNG bytes.
    # A worker's summary stays available alongside its logs for provenance.
    foreach($scaleDpi in $Dpi){
        $matrix=Join-Path $outputFull ($Width.ToString()+'x'+$Height+'-'+$scaleDpi+'dpi')
        [IO.Directory]::CreateDirectory($matrix) | Out-Null
        $aggregate=$null
        for($index=0;$index -lt [Math]::Min($Workers,$paths.Count);$index++){
            $source=Join-Path $outputFull ('.workers\'+$scaleDpi+'-worker'+$index)
            $summary=Get-Content -LiteralPath (Join-Path $source 'summary.json') -Raw | ConvertFrom-Json
            if($summary.engine -ne 'twebframe2' -or $summary.dpi -ne $scaleDpi){throw 'Worker capture provenance differs.'}
            if(!$aggregate){$aggregate=$summary}else{
                if($aggregate.actualWindowDpi -ne $summary.actualWindowDpi){throw 'Worker window DPI differs.'}
                $aggregate.attempted+=$summary.attempted;$aggregate.captured+=$summary.captured;$aggregate.errors+=$summary.errors
            }
            foreach($case in $cases){
                $caseSource=Join-Path $source $case.id
                if(Test-Path -LiteralPath $caseSource){
                    $resolved=(Resolve-Path -LiteralPath $caseSource).Path
                    if(!$resolved.StartsWith($outputFull+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Worker output is outside its run.'}
                    $destination=Join-Path $matrix $case.id
                    $destination=[IO.Path]::GetFullPath($destination)
                    if(!$destination.StartsWith($outputFull+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Worker destination is outside its run.'}
                    if(Test-Path -LiteralPath $destination){throw 'Duplicate worker document.'}
                    Move-Item -LiteralPath $resolved -Destination $destination
                }
            }
        }
        if($aggregate.attempted -ne $cases.Count -or $aggregate.captured+$aggregate.errors -ne $cases.Count){throw 'Worker matrix is incomplete.'}
        $aggregate | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $matrix 'summary.json') -Encoding UTF8
    }
}
$runMetadata.captureComplete=$true;$runMetadata.captureProcessFailures=$runErrors
$runMetadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputFull 'run.json') -Encoding UTF8
Write-Output "Capture output: $outputFull"
if($runErrors){throw 'Some captures failed; they remain failures in the run summary.'}
