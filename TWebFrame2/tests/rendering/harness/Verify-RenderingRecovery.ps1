param([Parameter(Mandatory=$true)][string]$RestoredRoot,[string]$EvidencePath='')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$RestoredRoot=[IO.Path]::GetFullPath($RestoredRoot)
if(-not $EvidencePath){$EvidencePath=Join-Path $RestoredRoot 'recovery-verification'}
$EvidencePath=[IO.Path]::GetFullPath($EvidencePath)
if(Test-Path -LiteralPath $EvidencePath){throw 'Recovery evidence exists; choose a new path'}
$run=Join-Path $RestoredRoot 'run'
$summary=Get-Content -LiteralPath (Join-Path $run 'summary.json') -Raw -Encoding UTF8|ConvertFrom-Json
$environment=Get-Content -LiteralPath (Join-Path $run 'environment.json') -Raw -Encoding UTF8|ConvertFrom-Json
$catalog=Get-Content -LiteralPath (Join-Path $RestoredRoot 'verified-archive-catalog.json') -Raw -Encoding UTF8|ConvertFrom-Json
[IO.Directory]::CreateDirectory($EvidencePath)|Out-Null
$snapshot=Join-Path $EvidencePath 'source'
[IO.Compression.ZipFile]::ExtractToDirectory((Join-Path $run 'source-snapshot.zip'),$snapshot)
foreach($source in $environment.sourceHashes){
    $file=[IO.Path]::GetFullPath((Join-Path $snapshot $source.file))
    if(-not $file.StartsWith($snapshot+'\',[StringComparison]::OrdinalIgnoreCase) -or (Get-FileHash -LiteralPath $file).Hash -cne $source.sha256){throw ('Restored source differs: '+$source.file)}
}
if((Get-FileHash -LiteralPath (Join-Path $run 'RenderingComparisonRegression.exe')).Hash -cne $environment.rendererExecutableSha256){throw 'Restored executable differs'}
foreach($runtime in $environment.runtimeHashes){if((Get-FileHash -LiteralPath (Join-Path $run $runtime.file)).Hash -cne $runtime.sha256){throw ('Restored runtime differs: '+$runtime.file)}}
$inputs=@(Get-Content -LiteralPath (Join-Path $run 'input-manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json)
foreach($case in $inputs){
    if($case.id -notmatch '^\d{4}-[a-z0-9-]+$'){throw 'Invalid restored input ID'}
    $folder=Join-Path $RestoredRoot ('inputs\'+$case.id)
    if((Get-FileHash -LiteralPath (Join-Path $folder 'index.html')).Hash -cne $case.htmlSha256 -or (Get-FileHash -LiteralPath (Join-Path $folder 'style.css')).Hash -cne $case.cssSha256){throw ('Restored input differs: '+$case.id)}
}
$compare=Join-Path $snapshot 'TWebFrame2\tests\rendering\harness\Compare-RenderingCapture.ps1'
$results=@()
foreach($row in $summary.results){
    if(-not $row.capturePath.StartsWith($summary.runPath.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid captured path'}
    $relative=$row.capturePath.Substring($summary.runPath.Length+1)
    $capture=Join-Path $run $relative
    $original=Get-Content -LiteralPath (Join-Path $capture 'result.json') -Raw -Encoding UTF8|ConvertFrom-Json
    $result=& $compare -CapturePath $capture -ComparisonPath (Join-Path $EvidencePath ('comparisons\'+$relative)) -StrictPixels:([bool]$summary.strictPixels)
    foreach($property in @('pass','status','strictPass','differentPixels','maximumChannelDelta','allowedBackendDifferencePixels')){
        if($result.$property -cne $original.$property){throw ('Restored comparison differs: '+$relative+' '+$property)}
    }
    if($original.referenceRoute.tested -and (-not $result.referenceRoute.tested -or $result.referenceRoute.matches -ne $original.referenceRoute.matches)){throw ('Restored capture route differs: '+$relative)}
    if($original.styleCoverage.contractVersion -eq 1){
        foreach($property in @('contractVersion','checked','missing','legacySkipped','includesNonRenderedElements')){
            if($result.styleCoverage.$property -cne $original.styleCoverage.$property){throw ('Restored style coverage differs: '+$relative+' '+$property)}
        }
    }
    $results+=@{id=$row.id;dpi=$row.dpi;viewport=$row.viewport;pass=$result.pass;strictPass=$result.strictPass;status=$result.status}
}
$record=[ordered]@{schemaVersion=1;archiveId=$catalog.archiveId;archiveSha256=$catalog.zipSha256;archiveEntriesVerified=$catalog.entryCount;restoredSourceHashesVerified=$environment.sourceHashes.Count;restoredInputsVerified=$inputs.Count;recomparedWithRestoredCode=$results.Count;passed=@($results|Where-Object pass).Count;strictlyPassed=@($results|Where-Object strictPass).Count;acceptedBackendDifferencePairs=@($results|Where-Object {$_.status -eq 'PASS_BACKEND_DIFFERENCE'}).Count;allRawMetricsAndStatusesMatch=$true;newRenderingExecuted=$false;evidencePath=$EvidencePath;results=$results}
[IO.File]::WriteAllText((Join-Path $EvidencePath 'recovery-validation.json'),($record|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
Write-Output ('Recovery verified: '+$results.Count+' comparisons, '+$environment.sourceHashes.Count+' source files; '+$EvidencePath)
