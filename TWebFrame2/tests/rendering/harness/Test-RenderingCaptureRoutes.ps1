param([Parameter(Mandatory=$true)][string]$RunPath)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Import-PixelComparison.ps1')
$RunPath=[IO.Path]::GetFullPath($RunPath)
$corpusRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixtureRoot=Join-Path $corpusRoot 'calibration'
$manifest=Get-Content -LiteralPath (Join-Path $fixtureRoot 'manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
$environment=Get-Content -LiteralPath (Join-Path $RunPath 'environment.json') -Raw -Encoding UTF8|ConvertFrom-Json
$executable=Join-Path $RunPath 'RenderingComparisonRegression.exe'
if((Get-FileHash -LiteralPath $executable).Hash -cne $environment.rendererExecutableSha256){throw 'Saved renderer hash mismatch'}
foreach($runtime in $environment.runtimeHashes){if((Get-FileHash -LiteralPath (Join-Path $RunPath $runtime.file)).Hash -cne $runtime.sha256){throw 'Saved runtime hash mismatch'}}
$sourceRoot=[IO.Path]::GetFullPath((Join-Path $corpusRoot '..\..\..'))
foreach($source in $environment.sourceHashes|Where-Object {$_.file -match 'rendering[\\/](calibration|harness)[\\/]'}){
    if((Get-FileHash -LiteralPath (Join-Path $sourceRoot $source.file)).Hash -cne $source.sha256){throw ('Calibration code or input differs from the saved run: '+$source.file)}
}
$outputRoot=Join-Path $RunPath 'capture-calibration'
if(Test-Path -LiteralPath $outputRoot){throw 'Calibration evidence exists; use a new run'}
[IO.Directory]::CreateDirectory($outputRoot)|Out-Null
[IO.File]::Copy((Join-Path $fixtureRoot 'manifest.json'),(Join-Path $outputRoot 'input-manifest.json'))
$encoding=[Text.UTF8Encoding]::new($false)
$folders=@()
foreach($document in $manifest.documents){
    if($document.id -notmatch '^[a-z0-9-]+$' -or $document.files.Count -ne 2){throw 'Invalid calibration manifest'}
    $folder=Join-Path $outputRoot ('inputs\'+$document.id)
    [IO.Directory]::CreateDirectory($folder)|Out-Null
    foreach($file in $document.files){
        if($file.path -notin @('index.html','style.css')){throw 'Invalid calibration input path'}
        $original=Join-Path $fixtureRoot ($document.id+'\'+$file.path)
        if((Get-FileHash -LiteralPath $original).Hash -cne $file.sha256){throw ('Calibration input changed: '+$document.id)}
        [IO.File]::Copy($original,(Join-Path $folder $file.path))
    }
    $folders+=$folder
}
$caseList=Join-Path $outputRoot 'cases.txt'
[IO.File]::WriteAllText($caseList,($folders -join "`n"),$encoding)
$results=@()
foreach($dpi in $environment.dpi){foreach($viewport in $environment.viewports){
    if($viewport -notmatch '^(\d+)x(\d+)$'){throw 'Invalid viewport'}
    $width=[int]$Matches[1];$height=[int]$Matches[2];$scale=$dpi/96.0
    $output=Join-Path $outputRoot ($dpi.ToString()+'dpi\'+$viewport)
    [IO.Directory]::CreateDirectory($output)|Out-Null
    & $executable --cases $caseList --measure (Join-Path $PSScriptRoot 'measure.js') --width $width --height $height --dpi $dpi --out $output 2>&1|Tee-Object -FilePath (Join-Path $output 'capture.log')
    $captureExit=$LASTEXITCODE
    [IO.File]::WriteAllText((Join-Path $output 'capture-exit.txt'),[string]$captureExit,$encoding)
    foreach($document in $manifest.documents){
        $capture=Join-Path $output $document.id
        $comparison=& (Join-Path $PSScriptRoot 'Compare-RenderingCapture.ps1') -CapturePath $capture -StrictPixels
        $checks=@();$errors=@()
        if($comparison.pass){
            foreach($name in @('reference.png','reference-cdp.png','native.png','native-window-paint.png')){
                $imagePath=Join-Path $capture $name
                if(-not (Test-Path -LiteralPath $imagePath)){
                    if($name -ne 'native-window-paint.png'){$errors+='Missing required calibration image: '+$name}
                    continue
                }
                $image=[Drawing.Bitmap]::new($imagePath)
                try{
                    if($image.Width -ne [Math]::Round($width*$scale) -or $image.Height -ne [Math]::Round($height*$scale)){$errors+='Wrong device-pixel dimensions: '+$name}
                    foreach($sample in $document.samples){
                        $x=if($null -ne $sample.right){$width-$sample.right}else{$sample.x}
                        $y=if($null -ne $sample.bottom){$height-$sample.bottom}else{$sample.y}
                        $color=$image.GetPixel([int][Math]::Floor($x*$scale),[int][Math]::Floor($y*$scale))
                        $actual=@([int]$color.R,[int]$color.G,[int]$color.B,[int]$color.A)
                        $pass=($actual -join ',') -ceq ($sample.rgba -join ',')
                        $checks+=@{image=$name;label=$sample.label;expected=$sample.rgba;actual=$actual;pass=$pass}
                        if(-not $pass){$errors+='Color sample differs: '+$name+' '+$sample.label}
                    }
                    foreach($span in $document.spans){
                        $start=[int][Math]::Floor($span.x*$scale)-1
                        $finish=[int][Math]::Ceiling(($span.x+$span.width)*$scale)+1
                        $y=[int][Math]::Floor($span.y*$scale);$pixels=@()
                        for($x=$start;$x -le $finish;$x++){
                            $color=$image.GetPixel($x,$y)
                            if($color.R -ne 255 -or $color.G -ne 255 -or $color.B -ne 255){$pixels+=$x}
                        }
                        $expected=[int][Math]::Ceiling($span.width*$scale)
                        $pass=$pixels.Count -eq $expected -and $pixels[0] -eq [int][Math]::Floor($span.x*$scale)
                        $checks+=@{image=$name;label=$span.label;expectedPixels=$expected;actualPixels=$pixels;pass=$pass}
                        if(-not $pass){$errors+='DPI ruler differs: '+$name+' '+$span.label}
                    }
                }finally{$image.Dispose()}
            }
        }
        $results+=@{id=$document.id;dpi=$dpi;viewport=$viewport;pass=($comparison.pass -and -not $errors.Count -and $captureExit -eq 0);status=$comparison.status;differentPixels=$comparison.differentPixels;referenceRoute=$comparison.referenceRoute;windowRoute=$comparison.windowRoute;checks=$checks;sampleErrors=$errors;failures=$comparison.failures}
    }
}}
$summary=[ordered]@{schemaVersion=1;scope=$manifest.scope;count=$results.Count;passed=@($results|Where-Object pass).Count;failed=@($results|Where-Object {-not $_.pass}).Count;transparentOutputValidated=$false;calibrated='Opaque final output: RGB channel order, alpha source-over compositing, 1/2/100 CSS-pixel widths, original viewport clipping and fixed positioning';results=$results}
[IO.File]::WriteAllText((Join-Path $outputRoot 'summary.json'),($summary|ConvertTo-Json -Depth 12),$encoding)
Write-Output ('Capture calibration: '+$summary.passed+'/'+$summary.count+' passed; '+$outputRoot)
if($summary.failed){exit 1}
