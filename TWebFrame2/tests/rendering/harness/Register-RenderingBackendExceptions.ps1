param([Parameter(Mandatory=$true)][string]$RunPath,
      [Parameter(Mandatory=$true)][string]$PreviousRunPath,
      [Parameter(Mandatory=$true)][string]$ApprovalReason,
      [switch]$AllCpuRasterization,
      [switch]$ReferenceEnvironmentChange,
      [string]$RegistryPath=(Join-Path $PSScriptRoot '../backend-pixel-exceptions.json'))
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Import-PixelComparison.ps1')
. (Join-Path $PSScriptRoot 'Read-RenderingGraphics.ps1')
$RunPath=[IO.Path]::GetFullPath($RunPath)
$PreviousRunPath=[IO.Path]::GetFullPath($PreviousRunPath)
$summary=Get-Content -Raw -LiteralPath (Join-Path $RunPath 'summary.json')|ConvertFrom-Json
$previous=Get-Content -Raw -LiteralPath (Join-Path $PreviousRunPath 'summary.json')|ConvertFrom-Json
$environment=Get-Content -Raw -LiteralPath (Join-Path $RunPath 'environment.json')|ConvertFrom-Json
$previousEnvironment=Get-Content -Raw -LiteralPath (Join-Path $PreviousRunPath 'environment.json')|ConvertFrom-Json
$priorApproval=$null
if($AllCpuRasterization -and $ReferenceEnvironmentChange){throw 'Select one backend transition mode'}
if(-not $AllCpuRasterization -and -not $ReferenceEnvironmentChange){$priorApproval=Get-Content -Raw -LiteralPath (Join-Path $PreviousRunPath 'task-acceptance.json')|ConvertFrom-Json}
$fontFiles=@($environment.fontHashes|ForEach-Object {$_.file+'|'+$_.sha256}|Sort-Object)
$oldFontFiles=@($previousEnvironment.fontHashes|ForEach-Object {$_.file+'|'+$_.sha256}|Sort-Object)
if($environment.windows -cne $previousEnvironment.windows -or
   @(Compare-Object $fontFiles $oldFontFiles).Count){
    throw 'CPU/GPU comparison requires the same Windows and font files'
}
$oldSources=@{};foreach($source in $previousEnvironment.sourceHashes){$oldSources[$source.file.Replace('\','/')]=$source.sha256}
$engineChanges=@($environment.sourceHashes|Where-Object {
    $name=$_.file.Replace('\','/')
    $name -match '^TWebFrame2/(src|include)/' -and $oldSources[$name] -cne $_.sha256
}|ForEach-Object {$_.file.Replace('\','/')})
if($ReferenceEnvironmentChange){
    if($engineChanges.Count){throw 'Reference environment classification requires unchanged engine sources'}
    foreach($source in $environment.sourceHashes|Where-Object {$_.file -match 'TWebFrame\.vcxproj|Directory.Build.targets$'}){
        if($oldSources[$source.file.Replace('\','/')] -cne $source.sha256){throw 'Engine build configuration changed'}
    }
    $calibration=Get-Content -Raw -LiteralPath (Join-Path $RunPath 'capture-calibration\summary.json')|ConvertFrom-Json
    if($calibration.count -ne 18 -or $calibration.failed -or $calibration.passed -ne 18){throw 'Reference environment classification requires the complete independent color/DPI calibration'}
    if($summary.count -ne 120 -or $summary.referenceCaptureRouteMatches -ne 120 -or
       $environment.referenceGraphicsRecordsVerified -ne 12 -or -not $environment.selectedReferenceGraphicsAdapterRecorded -or
       $environment.referenceGraphics.identity.featureStatus.featureStatus.gpu_compositing -ne 'enabled' -or
       $environment.referenceGraphics.identity.featureStatus.featureStatus.rasterization -ne 'enabled'){
        throw 'Reference environment classification requires a stable recorded GPU renderer and complete capture-route equality'
    }
    foreach($dpi in $environment.dpi){foreach($viewport in $environment.viewports){foreach($phase in @('before','after')){
        $graphics=Read-RenderingGraphicsIdentity (Join-Path $RunPath ($dpi.ToString()+'dpi\'+$viewport+'\reference-graphics-'+$phase+'.json'))
        if($graphics.fingerprint -cne $environment.referenceGraphics.fingerprint){throw 'Reference graphics identity differs from recorded environment'}
    }}}
    foreach($oldSource in $previousEnvironment.sourceHashes|Where-Object {$_.file.Replace('\','/') -match '^TWebFrame2/(src|include)/'}){
        $current=@($environment.sourceHashes|Where-Object {$_.file -ceq $oldSource.file})
        if($current.Count -ne 1 -or $current[0].sha256 -cne $oldSource.sha256){throw 'Previous engine sources are missing or changed'}
    }
}elseif($AllCpuRasterization){
    $expected=@('TWebFrame2/src/RasterSurface.h','TWebFrame2/src/RasterSkia.h','TWebFrame2/src/View.cpp','TWebFrame2/src/Layout.cpp')
    if($engineChanges.Count -ne $expected.Count -or @($engineChanges|Where-Object {$_ -notin $expected}).Count){
        throw 'CPU raster classification requires only the reviewed software geometry, Skia and View target changes'
    }
}elseif($engineChanges.Count -ne 1 -or $engineChanges[0] -cne 'TWebFrame2/src/RasterSurface.h'){
    throw 'Automatic font classification requires only the recorded CPU font composition change'
}
$oldRows=@{};foreach($row in $previous.results){$oldRows[$row.id+'|'+$row.dpi+'|'+$row.viewport]=$row}
if($ReferenceEnvironmentChange){
    foreach($row in $summary.results){
        $old=$oldRows[$row.id+'|'+$row.dpi+'|'+$row.viewport]
        if($null -eq $old -or -not $old.pass){throw 'Missing previously approved pair'}
        if([StrictRenderingPixels]::PixelSha256((Join-Path $row.capturePath 'native.png')) -cne [StrictRenderingPixels]::PixelSha256((Join-Path $old.capturePath 'native.png'))){throw 'Native pixels changed during reference environment classification'}
        $newMeasurement=Get-Content -LiteralPath (Join-Path $row.capturePath 'reference.json') -Raw|ConvertFrom-Json|ConvertTo-Json -Depth 20 -Compress
        $oldMeasurement=Get-Content -LiteralPath (Join-Path $old.capturePath 'reference.json') -Raw|ConvertFrom-Json|ConvertTo-Json -Depth 20 -Compress
        if($newMeasurement -cne $oldMeasurement){throw 'Reference DOM, computed style or geometry changed'}
        $result=Get-Content -Raw -LiteralPath (Join-Path $row.path 'result.json')|ConvertFrom-Json
        $oldResult=Get-Content -Raw -LiteralPath (Join-Path $old.path 'result.json')|ConvertFrom-Json
        if($result.runtime -cne $oldResult.runtime){throw 'WebView2 runtime changed; use a separately reviewed runtime transition'}
        if(@($result.failures|Where-Object {$_.kind -ne 'FAIL_PAINT'}).Count){throw 'Reference transition has non-paint failures'}
    }
}
$inputs=@{};foreach($entry in (Get-Content -Raw -LiteralPath (Join-Path $RunPath 'input-manifest.json')|ConvertFrom-Json)){$inputs[$entry.id]=$entry}
if($ReferenceEnvironmentChange){
    $oldInputs=@{};foreach($input in (Get-Content -Raw -LiteralPath (Join-Path $PreviousRunPath 'input-manifest.json')|ConvertFrom-Json)){$oldInputs[$input.id]=$input}
    foreach($input in $inputs.Values){if($input.htmlSha256 -cne $oldInputs[$input.id].htmlSha256 -or $input.cssSha256 -cne $oldInputs[$input.id].cssSha256){throw 'Input changed during reference environment classification'}}
}
$exceptions=[Collections.Generic.List[object]]::new()
if(Test-Path -LiteralPath $RegistryPath){
    $registry=Get-Content -Raw -LiteralPath $RegistryPath|ConvertFrom-Json
    foreach($entry in $registry.exceptions){$exceptions.Add($entry)}
}
$newCount=0
foreach($row in $summary.results){
    if(-not $row.differentPixels){continue}
    $result=Get-Content -Raw -LiteralPath (Join-Path $row.path 'result.json')|ConvertFrom-Json
    if(@($result.failures|Where-Object {$_.kind -ne 'FAIL_PAINT'}).Count){throw 'Non-paint failures cannot become backend exceptions'}
    $old=$oldRows[$row.id+'|'+$row.dpi+'|'+$row.viewport]
    if($null -eq $old){throw 'Missing GPU baseline pair'}
    $referenceHash=[StrictRenderingPixels]::PixelSha256((Join-Path $row.capturePath 'reference.png'))
    $nativeHash=[StrictRenderingPixels]::PixelSha256((Join-Path $row.capturePath 'native.png'))
    $oldReferenceHash=[StrictRenderingPixels]::PixelSha256((Join-Path $old.capturePath 'reference.png'))
    $oldNativeHash=[StrictRenderingPixels]::PixelSha256((Join-Path $old.capturePath 'native.png'))
    if(-not $ReferenceEnvironmentChange -and $referenceHash -cne $oldReferenceHash){throw 'Reference pixels changed during CPU/GPU classification'}
    if($ReferenceEnvironmentChange){
        $oldResult=Get-Content -Raw -LiteralPath (Join-Path $old.path 'result.json')|ConvertFrom-Json
        if(-not $oldResult.backendException -or $oldResult.backendException.kind -notin @('CPU_GPU_FONT_COMPOSITION','CPU_GPU_GRADIENT_COMPOSITION','CPU_GPU_RASTER_ANTIALIASING')){throw 'A newly failing feature cannot inherit a backend classification'}
        $kind=$oldResult.backendException.kind
    }elseif($AllCpuRasterization){
        if(-not $old.pass){throw 'CPU raster transition requires a previously passing or approved pair'}
        $oldResult=Get-Content -Raw -LiteralPath (Join-Path $old.path 'result.json')|ConvertFrom-Json
        if($nativeHash -ceq $oldNativeHash -and $oldResult.backendException){
            $kind=$oldResult.backendException.kind
        }else{
            $kind='CPU_GPU_RASTER_ANTIALIASING'
        }
    }elseif($old.pass){
        if($oldNativeHash -cne $oldReferenceHash -or $result.maximumChannelDelta -gt 2){throw 'Font change exceeds the observed blend quantization range'}
        $kind='CPU_GPU_FONT_COMPOSITION'
    }else{
        $approved=@($priorApproval.accepted|Where-Object {$_.id -ceq $row.id -and $_.dpi -eq $row.dpi -and $_.viewport -ceq $row.viewport})
        if($approved.Count -ne 1 -or $nativeHash -cne $oldNativeHash -or
           (Get-FileHash -LiteralPath (Join-Path $old.capturePath 'native.png')).Hash -cne $approved[0].nativePngSha256 -or
           (Get-FileHash -LiteralPath (Join-Path $old.capturePath 'reference.png')).Hash -cne $approved[0].referencePngSha256){
            throw 'Gradient exception differs from the previous user-approved pixels'
        }
        $kind='CPU_GPU_GRADIENT_COMPOSITION'
    }
    $entry=[ordered]@{caseId=$row.id;dpi=$row.dpi;viewport=$row.viewport;kind=$kind;
        referencePixelSha256=$referenceHash;nativePixelSha256=$nativeHash;
        differentPixels=$row.differentPixels;maximumChannelDelta=$result.maximumChannelDelta;
        htmlSha256=$inputs[$row.id].htmlSha256;cssSha256=$inputs[$row.id].cssSha256;
        gpuBaselineRun=(Split-Path $PreviousRunPath -Leaf);cpuEvidenceRun=(Split-Path $RunPath -Leaf);
        approval='User instruction, 2026-10-04';reason=$ApprovalReason}
    if($ReferenceEnvironmentChange){$entry.referenceGraphicsFingerprint=$environment.referenceGraphics.fingerprint;$entry.classification='Previously approved CPU/GPU feature; all 120 native pixels and reference DOM/styles/geometry unchanged; independent capture/color/DPI calibration passed in separately recorded reference GPU environment'}
    $exists=@($exceptions|Where-Object {$_.caseId -ceq $entry.caseId -and $_.dpi -eq $entry.dpi -and $_.viewport -ceq $entry.viewport -and $_.referencePixelSha256 -ceq $referenceHash -and $_.nativePixelSha256 -ceq $nativeHash})
    if(-not $exists.Count){$exceptions.Add([pscustomobject]$entry);++$newCount}
}
$registry=[ordered]@{schemaVersion=1;policyId='exact-pixels-with-approved-cpu-gpu-exceptions-v1';
    rule='Exact decoded pixels by default. Approved font, gradient and raster antialiasing CPU/GPU pixel signatures pass only after all structural and capture checks pass.';
    globalChannelTolerance=0;engineChangesVerified=$engineChanges;exceptions=@($exceptions.ToArray())}
[IO.File]::WriteAllText([IO.Path]::GetFullPath($RegistryPath),($registry|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
Write-Output ('Registered '+$newCount+' CPU/GPU pixel exceptions; '+$exceptions.Count+' total. '+$RegistryPath)
