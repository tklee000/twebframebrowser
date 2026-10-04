param([Parameter(Mandatory=$true)][string]$RunPath)
$ErrorActionPreference='Stop'
$RunPath=[IO.Path]::GetFullPath($RunPath)
$corpusRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sourceRoot=[IO.Path]::GetFullPath((Join-Path $corpusRoot '..\..\..'))
$fixtureRoot=Join-Path $corpusRoot 'style-calibration'
$manifest=Get-Content -LiteralPath (Join-Path $fixtureRoot 'manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
$environment=Get-Content -LiteralPath (Join-Path $RunPath 'environment.json') -Raw -Encoding UTF8|ConvertFrom-Json
$executable=Join-Path $RunPath 'RenderingComparisonRegression.exe'
if((Get-FileHash -LiteralPath $executable).Hash -cne $environment.rendererExecutableSha256){throw 'Saved renderer hash mismatch'}
foreach($runtime in $environment.runtimeHashes){if((Get-FileHash -LiteralPath (Join-Path $RunPath $runtime.file)).Hash -cne $runtime.sha256){throw 'Saved runtime hash mismatch'}}
foreach($source in $environment.sourceHashes|Where-Object {$_.file -match 'rendering[\\/](style-calibration|harness)[\\/]'}){
    if((Get-FileHash -LiteralPath (Join-Path $sourceRoot $source.file)).Hash -cne $source.sha256){throw ('Style calibration differs from the saved source: '+$source.file)}
}
$outputRoot=Join-Path $RunPath 'style-calibration'
if(Test-Path -LiteralPath $outputRoot){throw 'Style calibration already exists; preserve it'}
[IO.Directory]::CreateDirectory($outputRoot)|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
[IO.File]::Copy((Join-Path $fixtureRoot 'manifest.json'),(Join-Path $outputRoot 'input-manifest.json'))
$folders=@()
foreach($document in $manifest.documents){
    if($document.id -notmatch '^[a-z0-9-]+$' -or $document.files.Count -ne 2){throw 'Invalid style calibration document'}
    $folder=Join-Path $outputRoot ('inputs\'+$document.id);[IO.Directory]::CreateDirectory($folder)|Out-Null
    foreach($file in $document.files){
        if($file.path -notin @('index.html','style.css')){throw 'Invalid style calibration path'}
        $original=Join-Path $fixtureRoot ($document.id+'\'+$file.path)
        if((Get-FileHash -LiteralPath $original).Hash -cne $file.sha256){throw 'Style calibration input changed'}
        [IO.File]::Copy($original,(Join-Path $folder $file.path))
    }
    $folders+=$folder
}
$caseList=Join-Path $outputRoot 'cases.txt';[IO.File]::WriteAllText($caseList,($folders -join "`n"),$encoding)
$results=@()
. (Join-Path $PSScriptRoot 'Compare-RenderingStyles.ps1')
foreach($dpi in $environment.dpi){foreach($viewport in $environment.viewports){
    if($viewport -notmatch '^(\d+)x(\d+)$'){throw 'Invalid viewport'}
    $width=[int]$Matches[1];$height=[int]$Matches[2]
    $output=Join-Path $outputRoot ($dpi.ToString()+'dpi\'+$viewport);[IO.Directory]::CreateDirectory($output)|Out-Null
    & $executable --cases $caseList --measure (Join-Path $PSScriptRoot 'measure.js') --width $width --height $height --dpi $dpi --out $output 2>&1|Tee-Object -FilePath (Join-Path $output 'capture.log')
    $captureExit=$LASTEXITCODE
    [IO.File]::WriteAllText((Join-Path $output 'capture-exit.txt'),[string]$captureExit,$encoding)
    foreach($document in $manifest.documents){
        $capture=Join-Path $output $document.id
        $comparison=& (Join-Path $PSScriptRoot 'Compare-RenderingCapture.ps1') -CapturePath $capture -StrictPixels
        $checks=@()
        foreach($engine in @('reference','native')){
            $diagnostics=Get-Content -LiteralPath (Join-Path $capture ($engine+'.json')) -Raw -Encoding UTF8|ConvertFrom-Json
            foreach($expectation in $document.expected){
                $box=@($diagnostics.boxes|Where-Object {$_.id -ceq $expectation.id})
                if($box.Count -ne 1){throw 'Expected style calibration node is missing or duplicated'}
                foreach($property in $expectation.styles.PSObject.Properties){
                    $expectedValue=[string]$property.Value;$actual=[string]$box[0].computedStyles.($property.Name)
                    $pass=$expectedValue -ceq $actual
                    if($property.Name -in @('color','background-color')){$pass=(Convert-RenderingStyleColor $expectedValue) -ceq (Convert-RenderingStyleColor $actual)}
                    $checks+=@{engine=$engine;id=$expectation.id;property=$property.Name;expected=$expectedValue;actual=$actual;pass=$pass}
                }
                if($null -ne $expectation.PSObject.Properties['present']){$checks+=@{engine=$engine;id=$expectation.id;property='present';expected=$expectation.present;actual=$box[0].present;pass=($box[0].present -eq $expectation.present)}}
            }
        }
        $expectedValuesPass=-not @($checks|Where-Object {-not $_.pass}).Count
        $structuralPass=$captureExit -eq 0 -and -not @($comparison.failures|Where-Object {$_.kind -ne 'FAIL_PAINT'}).Count
        $stylePass=$structuralPass -and $expectedValuesPass -and $comparison.styleCoverage.contractVersion -eq 1 -and $comparison.styleCoverage.missing -eq 0
        $results+=@{id=$document.id;dpi=$dpi;viewport=$viewport;pass=($comparison.pass -and $expectedValuesPass);stylePass=$stylePass;strictPass=$comparison.strictPass;styleChecked=$comparison.styleCoverage.checked;styleMissing=$comparison.styleCoverage.missing;status=$comparison.status;differentPixels=$comparison.differentPixels;maximumChannelDelta=$comparison.maximumChannelDelta;checks=$checks;failures=$comparison.failures}
    }
}}
$summary=[ordered]@{schemaVersion=1;scope=$manifest.scope;count=$results.Count;passed=@($results|Where-Object pass).Count;failed=@($results|Where-Object {-not $_.pass}).Count;stylePassed=@($results|Where-Object stylePass).Count;strictlyPassed=@($results|Where-Object strictPass).Count;expectedValueChecks=@($results.checks).Count;unapprovedPaintDifferencesRemain=(@($results|Where-Object {-not $_.pass}).Count -gt 0);results=$results}
[IO.File]::WriteAllText((Join-Path $outputRoot 'summary.json'),($summary|ConvertTo-Json -Depth 12),$encoding)
Write-Output ('Style calibration: full comparison '+$summary.passed+'/'+$summary.count+'; styles/structure '+$summary.stylePassed+'/'+$summary.count+'; expected values '+$summary.expectedValueChecks)
if($summary.failed){exit 1}
