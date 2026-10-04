param([Parameter(Mandatory=$true)][string]$RunPath,[int[]]$Dpi=@(96,144),[string]$Viewport='960x660')
$ErrorActionPreference='Stop'
$RunPath=[IO.Path]::GetFullPath($RunPath)
$corpusRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$reproRoot=Join-Path $corpusRoot 'repros'
$outputRoot=Join-Path $RunPath 'repro-evidence'
if(Test-Path -LiteralPath $outputRoot){throw 'Repro evidence exists; use a new run to preserve results'}
if($Viewport -notmatch '^(\d+)x(\d+)$'){throw 'Invalid viewport'}
$width=[int]$Matches[1];$height=[int]$Matches[2]
if($width -lt 1 -or $height -lt 1 -or $width -gt 8192 -or $height -gt 8192){throw 'Invalid viewport size'}
if(-not $Dpi.Count -or @($Dpi|Select-Object -Unique).Count -ne $Dpi.Count){throw 'Empty or duplicate DPI matrix'}
foreach($value in $Dpi){if($value -notin @(96,144)){throw 'DPI must be 96 or 144'}}
$environment=Get-Content -LiteralPath (Join-Path $RunPath 'environment.json') -Raw -Encoding UTF8|ConvertFrom-Json
$executable=Join-Path $RunPath 'RenderingComparisonRegression.exe'
if((Get-FileHash -LiteralPath $executable).Hash -cne $environment.rendererExecutableSha256){throw 'Saved renderer hash mismatch'}
foreach($runtime in $environment.runtimeHashes){
    if((Get-FileHash -LiteralPath (Join-Path $RunPath $runtime.file)).Hash -cne $runtime.sha256){throw ('Saved runtime hash mismatch: '+$runtime.file)}
}
$measure=Join-Path $PSScriptRoot 'measure.js'
$measurementHash=@($environment.sourceHashes|Where-Object {$_.file -match 'rendering[\\/]harness[\\/]measure\.js$'})
if($measurementHash.Count -ne 1 -or (Get-FileHash -LiteralPath $measure).Hash -cne $measurementHash[0].sha256){throw 'Measurement script differs from captured run'}
$manifest=Get-Content -LiteralPath (Join-Path $reproRoot 'manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
if($manifest.documentCount -ne @($manifest.documents).Count){throw 'Repro manifest documentCount differs from its document list'}
foreach($document in $manifest.documents){
    if($document.id -notmatch '^[a-z0-9-]+$'){throw 'Invalid repro ID'}
    foreach($file in $document.files){
        if($file.path -notin @('index.html','style.css')){throw 'Invalid repro filename'}
        if((Get-FileHash -LiteralPath (Join-Path $reproRoot ($document.id+'\'+$file.path))).Hash -cne $file.sha256){throw ('Repro input changed: '+$document.id)}
    }
}
[IO.Directory]::CreateDirectory($outputRoot)|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
$caseList=Join-Path $outputRoot 'cases.txt'
[IO.File]::WriteAllText($caseList,(($manifest.documents|ForEach-Object {Join-Path $reproRoot $_.id}) -join "`n"),$encoding)
[IO.File]::Copy((Join-Path $reproRoot 'manifest.json'),(Join-Path $outputRoot 'input-manifest.json'))
$results=[Collections.Generic.List[object]]::new()
foreach($value in $Dpi){
    $output=Join-Path $outputRoot ($value.ToString()+'dpi\'+$Viewport)
    [IO.Directory]::CreateDirectory($output)|Out-Null
    $previousPreference=$ErrorActionPreference
    try{
        $ErrorActionPreference='Continue'
        & $executable --cases $caseList --measure $measure --width $width --height $height --dpi $value --out $output 2>&1|Tee-Object -FilePath (Join-Path $output 'capture.log')
        $captureExit=$LASTEXITCODE
    }finally{$ErrorActionPreference=$previousPreference}
    [IO.File]::WriteAllText((Join-Path $output 'capture-exit.txt'),[string]$captureExit,$encoding)
    foreach($document in $manifest.documents){
        $capture=Join-Path $output $document.id
        $comparison=& (Join-Path $PSScriptRoot 'Compare-RenderingCapture.ps1') -CapturePath $capture
        $results.Add([pscustomobject]@{id=$document.id;dpi=$value;viewport=$Viewport;status=$comparison.status;pass=$comparison.pass;differentPixels=$comparison.differentPixels;path=$capture;failures=$comparison.failures})
    }
}
$passed=@($results|Where-Object pass).Count
$summary=[ordered]@{schemaVersion=1;scope='Reduced diagnostic documents; excluded from the base corpus count';count=$results.Count;passed=$passed;failed=$results.Count-$passed;fullContractComplete=$false;results=@($results.ToArray())}
[IO.File]::WriteAllText((Join-Path $outputRoot 'summary.json'),($summary|ConvertTo-Json -Depth 18),$encoding)
Write-Output ('Repros: '+$passed+'/'+$results.Count+' passed; '+$outputRoot)
if($passed -ne $results.Count){exit 1}
