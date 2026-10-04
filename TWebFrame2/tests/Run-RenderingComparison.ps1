param([int]$Count=20,[int[]]$Dpi=@(96,144),[string[]]$Viewports=@('384x768','800x600','1280x800'),[string[]]$CaseId=@(),[switch]$SkipBuild,[switch]$CompareOnly,[switch]$StrictPixels,[string]$RunPath='',
    [string]$MSBuild='C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe')
$ErrorActionPreference='Stop'
$repoRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$corpusRoot=Join-Path $PSScriptRoot 'rendering'
$encoding=[Text.UTF8Encoding]::new($false)
. (Join-Path $corpusRoot 'harness\Read-RenderingGraphics.ps1')
if(-not $Dpi.Count -or -not $Viewports.Count){throw 'DPI and viewport matrices must not be empty'}
$manifest=Get-Content -LiteralPath (Join-Path $corpusRoot 'corpus-manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
if($Count -lt 1 -or ($Count -gt $manifest.count -and -not $CaseId.Count)){throw 'Count exceeds the preserved corpus; generate the next stage explicitly'}
$cases=@($manifest.cases|Select-Object -First $Count)
if($CaseId.Count){
    foreach($id in $CaseId){if($id -notin $manifest.cases.id){throw ('Unknown case ID: '+$id)}}
    $cases=@($manifest.cases|Where-Object{$CaseId -contains $_.id})
}
if(@($Dpi|Select-Object -Unique).Count -ne $Dpi.Count -or @($Viewports|Select-Object -Unique).Count -ne $Viewports.Count){throw 'Duplicate matrix entries would overwrite captured results'}
foreach($scaleDpi in $Dpi){if($scaleDpi -notin @(96,144)){throw 'DPI must be 96 or 144'}}
foreach($viewport in $Viewports){if($viewport -notmatch '^(\d+)x(\d+)$' -or [int]$Matches[1] -lt 1 -or [int]$Matches[2] -lt 1 -or [int]$Matches[1] -gt 8192 -or [int]$Matches[2] -gt 8192){throw ('Invalid viewport: '+$viewport)}}
if(-not $cases.Count){throw 'No cases selected'}
foreach($case in $cases){
    $folder=Join-Path $corpusRoot ('fixtures\'+$case.id)
    if((Get-FileHash -LiteralPath (Join-Path $folder 'index.html')).Hash -ne $case.htmlSha256 -or (Get-FileHash -LiteralPath (Join-Path $folder 'style.css')).Hash -ne $case.cssSha256){throw ('Input hash mismatch: '+$case.id)}
}
if(-not $CompareOnly -and -not $SkipBuild){
    & $MSBuild (Join-Path $PSScriptRoot 'RenderingComparisonRegression.vcxproj') /t:Build /p:Configuration=Release /p:Platform=x64 /m:2 /v:minimal /nologo
    if($LASTEXITCODE -ne 0){throw 'Rendering comparison build failed'}
}
if($CompareOnly -and -not $RunPath){throw 'CompareOnly requires an existing RunPath'}
if(-not $RunPath){$RunPath=Join-Path $corpusRoot ('runs\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))}
$RunPath=[IO.Path]::GetFullPath($RunPath)
if(-not $CompareOnly -and (Test-Path -LiteralPath $RunPath)){throw 'Run directory exists; use a new directory to preserve results'}
[IO.Directory]::CreateDirectory($RunPath)|Out-Null
$reportRoot=$RunPath
if($CompareOnly){$reportRoot=Join-Path $RunPath ('comparisons\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8));[IO.Directory]::CreateDirectory($reportRoot)|Out-Null}
$runClock=[Diagnostics.Stopwatch]::StartNew()
if(-not $CompareOnly){
    & (Join-Path $corpusRoot 'harness\Test-RenderingComparison.ps1') -OutputPath (Join-Path $RunPath 'comparison-controls')
    $caseList=Join-Path $RunPath 'cases.txt'
    [IO.File]::WriteAllText($caseList,(($cases|ForEach-Object{[IO.Path]::GetFullPath((Join-Path $corpusRoot ('fixtures\'+$_.id)))}) -join "`n"),$encoding)
    $gitSha=(& git -C $repoRoot rev-parse HEAD|Out-String).Trim()
    $gitStatus=(& git -C $repoRoot status --short|Out-String).Trim()
    $sourceFiles=@(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'TWebFrame2\src') -File)+@(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'TWebFrame2\include') -File -Recurse)+@(Get-Item -LiteralPath (Join-Path $PSScriptRoot 'RenderingComparisonRegression.cpp'),(Join-Path $PSScriptRoot 'RenderingComparisonRegression.vcxproj'),(Join-Path $PSScriptRoot 'RegressionIO.h'),(Join-Path $PSScriptRoot 'Run-RenderingComparison.ps1'),(Join-Path $repoRoot 'TWebFrame2\TWebFrame.vcxproj'),(Join-Path $corpusRoot 'comparison-contract.json'),(Join-Path $corpusRoot 'corpus-manifest.json'),(Join-Path $corpusRoot 'feature-inventory.json'),(Join-Path $corpusRoot 'coverage.json'))+@(Get-ChildItem -LiteralPath (Join-Path $corpusRoot 'harness') -File)+@(Get-ChildItem -LiteralPath (Join-Path $corpusRoot 'generator') -File)+@(Get-ChildItem -LiteralPath (Join-Path $corpusRoot 'schemas') -File)+@(Get-Item -LiteralPath (Join-Path $PSScriptRoot 'TWebFrameTests.cpp'),(Join-Path $PSScriptRoot 'TableSpanRegression.cpp'),(Join-Path $PSScriptRoot 'FormControlRegression.cpp'),(Join-Path $PSScriptRoot 'FormControlRegression.vcxproj'),(Join-Path $PSScriptRoot 'TWebFrameTests.vcxproj'),(Join-Path $PSScriptRoot 'TableSpanRegression.vcxproj'),(Join-Path $PSScriptRoot 'SupportBuild.props'),(Join-Path $PSScriptRoot 'platform-integrity-regression.js'),(Join-Path $repoRoot 'TWebFrame2\TWebFrame.vcxproj.filters'),(Join-Path $PSScriptRoot 'Run-SupportCompatibility.ps1'))
    $sourceFiles+=@(Get-Item -LiteralPath (Join-Path $repoRoot 'Directory.Build.targets'))
    $sourceFiles+=@(Get-ChildItem -LiteralPath (Join-Path $corpusRoot 'calibration') -File -Recurse)
    $sourceFiles+=@(Get-ChildItem -LiteralPath (Join-Path $corpusRoot 'style-calibration') -File -Recurse)
    $sourceFiles+=@(Get-Item -LiteralPath (Join-Path $PSScriptRoot 'ScrollRenderingRegression.cpp'),(Join-Path $PSScriptRoot 'ScrollRenderingRegression.vcxproj'))
    if(Test-Path -LiteralPath (Join-Path $corpusRoot 'backend-pixel-exceptions.json')){
        $sourceFiles+=@(Get-Item -LiteralPath (Join-Path $corpusRoot 'backend-pixel-exceptions.json'))
    }
    $sourceFiles+=@(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'TWebFrame2\vendor\skia') -File)
    $sourceHashes=@($sourceFiles|Sort-Object FullName|ForEach-Object{@{file=$_.FullName.Substring($repoRoot.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}})
    $runtimeFiles=@('libSkiaSharp.dll','LICENSE.txt','THIRD-PARTY-NOTICES.txt')
    $runtimeHashes=@(foreach($runtimeFile in $runtimeFiles){
        $vendorFile=Join-Path $repoRoot ('TWebFrame2\vendor\skia\'+$runtimeFile)
        $deployedFile=Join-Path $PSScriptRoot ('bin\x64\Release\'+$runtimeFile)
        $runtimeHash=(Get-FileHash -LiteralPath $vendorFile).Hash
        if((Get-FileHash -LiteralPath $deployedFile).Hash -cne $runtimeHash){throw ('Runtime deployment differs from source: '+$runtimeFile)}
        [IO.File]::Copy($deployedFile,(Join-Path $RunPath $runtimeFile))
        @{file=$runtimeFile;sha256=$runtimeHash}
    })
    $fontHashes=@('arial.ttf','arialbd.ttf','ariali.ttf','arialbi.ttf','malgun.ttf','malgunbd.ttf','segoeui.ttf','segoeuib.ttf','SegUIVar.ttf','seguisym.ttf','seguiemj.ttf','tahoma.ttf','tahomabd.ttf','times.ttf','timesbd.ttf','timesi.ttf','timesbi.ttf','consola.ttf','consolab.ttf','consolai.ttf','consolaz.ttf','gulim.ttc','NotoSansKR-VF.ttf'|ForEach-Object{$fontFile=Join-Path $env:WINDIR ('Fonts\'+$_);if(Test-Path -LiteralPath $fontFile){@{file=$_;sha256=(Get-FileHash -LiteralPath $fontFile).Hash}}})
    $environment=[ordered]@{schemaVersion=1;createdUtc=[DateTime]::UtcNow.ToString('o');gitSha=$gitSha;workingTree=$gitStatus;sourceHashes=$sourceHashes;fontHashes=$fontHashes;windows=[Environment]::OSVersion.VersionString;sdk='1.0.3595.46';dpi=$Dpi;viewports=$Viewports;pageZoom=1.0;dpiRoute='explicit-dpi-view-paint';actualWindowsBothDpiValidated=$false;fixtureCount=$cases.Count;rendererExecutableSha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'bin\x64\Release\RenderingComparisonRegression.exe')).Hash}
    $environment.fontLocale=[ordered]@{culture=(Get-Culture).Name;uiCulture=(Get-UICulture).Name}
    $environment.referenceCaptureRoutes=@('CapturePreview','Page.captureScreenshot')
    $environment.referenceCaptureCalibrationRequired=$true
    $environment.computedStyleContractVersion=1
    $environment.computedStylePropertyCount=25
    $environment.runtimeHashes=$runtimeHashes
    $environment.rasterBackend='CPU only: software Direct2D WIC geometry and LCD glyph composition; Skia raster-direct gradients, solid corners and shadows; software HWND/DC presentation; no GPU masks, compositing or readback'
    $environment.enumeratedGraphicsAdapters=@(Get-CimInstance Win32_VideoController|ForEach-Object {@{name=$_.Name;driverVersion=$_.DriverVersion;deviceId=$_.PNPDeviceID}})
    $environment.selectedGraphicsAdaptersRecorded=$false
    [IO.File]::WriteAllText((Join-Path $RunPath 'environment.json'),($environment|ConvertTo-Json -Depth 10),$encoding)
    [IO.File]::WriteAllText((Join-Path $RunPath 'input-manifest.json'),(ConvertTo-Json -InputObject $cases -Depth 10),$encoding)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $sourceArchive=[IO.Compression.ZipFile]::Open((Join-Path $RunPath 'source-snapshot.zip'),[IO.Compression.ZipArchiveMode]::Create)
    try{foreach($sourceFile in $sourceFiles){[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($sourceArchive,$sourceFile.FullName,$sourceFile.FullName.Substring($repoRoot.Length+1).Replace('\','/'),[IO.Compression.CompressionLevel]::Optimal)|Out-Null}}
    finally{$sourceArchive.Dispose()}
    [IO.File]::Copy((Join-Path $PSScriptRoot 'bin\x64\Release\RenderingComparisonRegression.exe'),(Join-Path $RunPath 'RenderingComparisonRegression.exe'))
    foreach($scaleDpi in $Dpi){foreach($viewport in $Viewports){
        if($scaleDpi -notin @(96,144) -or $viewport -notmatch '^(\d+)x(\d+)$'){throw 'Invalid DPI/viewport'}
        $width=$Matches[1];$height=$Matches[2];$output=Join-Path $RunPath ($scaleDpi.ToString()+'dpi\'+$viewport)
        [IO.Directory]::CreateDirectory($output)|Out-Null
        $nativeErrorPreference=$ErrorActionPreference
        try{
            $ErrorActionPreference='Continue'
            & (Join-Path $PSScriptRoot 'bin\x64\Release\RenderingComparisonRegression.exe') --cases $caseList --measure (Join-Path $corpusRoot 'harness\measure.js') --width $width --height $height --dpi $scaleDpi --out $output 2>&1|Tee-Object -FilePath (Join-Path $output 'capture.log')
            $captureExit=$LASTEXITCODE
        }finally{$ErrorActionPreference=$nativeErrorPreference}
        [IO.File]::WriteAllText((Join-Path $output 'capture-exit.txt'),[string]$captureExit,$encoding)
    }}
    $graphicsRecords=@(foreach($scaleDpi in $Dpi){foreach($viewport in $Viewports){foreach($phase in @('before','after')){
        Read-RenderingGraphicsIdentity (Join-Path $RunPath ($scaleDpi.ToString()+'dpi\'+$viewport+'\reference-graphics-'+$phase+'.json'))
    }}})
    if(@($graphicsRecords.fingerprint|Select-Object -Unique).Count -ne 1){throw 'Reference graphics environment changed during capture; preserved results require investigation'}
    $environment.referenceGraphics=$graphicsRecords[0]
    $environment.referenceGraphicsRecordsVerified=$graphicsRecords.Count
    $environment.selectedReferenceGraphicsAdapterRecorded=$true
    [IO.File]::WriteAllText((Join-Path $RunPath 'environment.json'),($environment|ConvertTo-Json -Depth 16),$encoding)
}
$results=[Collections.Generic.List[object]]::new()
foreach($scaleDpi in $Dpi){foreach($viewport in $Viewports){foreach($case in $cases){
    $capturePath=Join-Path $RunPath ($scaleDpi.ToString()+'dpi\'+$viewport+'\'+$case.id)
    if(Test-Path -LiteralPath (Join-Path $capturePath 'capture-status.json')){
        $comparisonPath=Join-Path $reportRoot ($scaleDpi.ToString()+'dpi\'+$viewport+'\'+$case.id)
        try{$comparison=& (Join-Path $corpusRoot 'harness\Compare-RenderingCapture.ps1') -CapturePath $capturePath -ComparisonPath $comparisonPath -StrictPixels:$StrictPixels}
        catch{
            $comparison=[pscustomobject]@{schemaVersion=1;pass=$false;status='HARNESS_ERROR';differentPixels=$null;fullContractComplete=$false;failures=@(@{kind='HARNESS_ERROR';message=$_.Exception.Message})}
            [IO.Directory]::CreateDirectory($comparisonPath)|Out-Null
            $errorFile=Join-Path $comparisonPath 'comparison-error.json'
            if(Test-Path -LiteralPath $errorFile){throw 'Comparison error file already exists'}
            [IO.File]::WriteAllText($errorFile,($comparison|ConvertTo-Json -Depth 10),$encoding)
        }
        $results.Add([pscustomobject]@{id=$case.id;dpi=$scaleDpi;viewport=$viewport;pass=$comparison.pass;status=$comparison.status;strictPass=$comparison.strictPass;differentPixels=$comparison.differentPixels;referenceRouteMatched=($comparison.referenceRoute.tested -and $comparison.referenceRoute.matches);styleContractVersion=$comparison.styleCoverage.contractVersion;styleChecked=$comparison.styleCoverage.checked;styleMissing=$comparison.styleCoverage.missing;allowedBackendDifferencePixels=$comparison.allowedBackendDifferencePixels;backendExceptionKind=$comparison.backendException.kind;path=$comparisonPath;capturePath=$capturePath;failures=$comparison.failures})
    }else{$results.Add([pscustomobject]@{id=$case.id;dpi=$scaleDpi;viewport=$viewport;pass=$false;status='PENDING';differentPixels=$null;path=$capturePath})}
}}}
$passCount=@($results|Where-Object pass).Count
$runClock.Stop()
$summary=[ordered]@{schemaVersion=1;count=$results.Count;passed=$passCount;failed=$results.Count-$passCount;strictlyPassed=@($results|Where-Object strictPass).Count;acceptedBackendDifferencePairs=@($results|Where-Object {$_.status -eq 'PASS_BACKEND_DIFFERENCE'}).Count;strictPixels=[bool]$StrictPixels;passScope='authored DOM, tracked box rects, versioned 25-property required computed styles when captured, UTF-16 character rects when captured; exact BGRA with approved CPU/GPU font/gradient/raster AA exceptions; full contract pending';fullContractComplete=$false;actualWindowsBothDpiValidated=$false;runPath=$RunPath;reportPath=$reportRoot;elapsedSeconds=$runClock.Elapsed.TotalSeconds;results=@($results.ToArray())}
$pixelPolicyPath=Join-Path $corpusRoot 'backend-pixel-exceptions.json'
$summary.referenceCaptureRouteMatches=@($results|Where-Object referenceRouteMatched).Count
$summary.requiredComputedStyleCaptures=@($results|Where-Object {$_.styleContractVersion -eq 1}).Count
$summary.requiredComputedStyleValuesChecked=($results|Measure-Object styleChecked -Sum).Sum
$summary.requiredComputedStyleValuesMissing=($results|Measure-Object styleMissing -Sum).Sum
if(Test-Path -LiteralPath $pixelPolicyPath){[IO.File]::Copy($pixelPolicyPath,(Join-Path $reportRoot 'backend-pixel-exceptions.json'));$summary.backendExceptionRegistrySha256=(Get-FileHash -LiteralPath $pixelPolicyPath).Hash}
[IO.File]::WriteAllText((Join-Path $reportRoot 'summary.json'),($summary|ConvertTo-Json -Depth 18),$encoding)
[IO.File]::WriteAllText((Join-Path $reportRoot 'failures.json'),(ConvertTo-Json -InputObject @($results|Where-Object{-not $_.pass}) -Depth 18),$encoding)
$rows=($results|ForEach-Object{'<tr><td>'+[Net.WebUtility]::HtmlEncode($_.id)+'</td><td>'+ $_.dpi+'</td><td>'+ $_.viewport+'</td><td>'+ $_.status+'</td><td>'+ $_.differentPixels+'</td><td><a href="'+[Net.WebUtility]::HtmlEncode(($_.path.Substring($reportRoot.Length+1)+'\diff.png').Replace('\','/'))+'">diff</a></td></tr>'}) -join "`n"
[IO.File]::WriteAllText((Join-Path $reportRoot 'summary.html'),('<!doctype html><meta charset="utf-8"><title>Rendering comparison</title><style>body{font:14px Arial;padding:20px}td,th{padding:6px;border:1px solid #ccc}table{border-collapse:collapse}</style><h1>Rendering comparison</h1><p>Passed '+$passCount+' / '+$results.Count+'. Exact pixels: '+$summary.strictlyPassed+'; approved CPU/GPU differences: '+$summary.acceptedBackendDifferencePairs+'. Full contract pending.</p><table><tr><th>Case</th><th>DPI</th><th>Viewport</th><th>Status</th><th>Different pixels</th><th>Artifact</th></tr>'+$rows+'</table>'),$encoding)
Write-Output ('Rendering: '+$passCount+'/'+$results.Count+' passed; '+$reportRoot)
if($passCount -ne $results.Count){exit 1}
