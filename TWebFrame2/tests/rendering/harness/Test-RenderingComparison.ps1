param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Import-PixelComparison.ps1')
. (Join-Path $PSScriptRoot 'Compare-RenderingText.ps1')
. (Join-Path $PSScriptRoot 'Compare-RenderingBackend.ps1')
if(Test-Path -LiteralPath $OutputPath){throw 'Calibration output exists; preserve it and use a new path'}
[IO.Directory]::CreateDirectory($OutputPath)|Out-Null
function Picture([string]$Path,[int]$Shift=0,[int]$Delta=0,[switch]$Missing,[switch]$Clip,[string]$FontName='Arial',[int]$TextShift=0,[int]$TextDelta=0,[switch]$Wrap,[switch]$TextRegionError,[switch]$AlphaError,[switch]$WrongOrder){
    $bitmap=[Drawing.Bitmap]::new(96,64,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics=[Drawing.Graphics]::FromImage($bitmap)
    $brush=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255,40+$Delta,100,180))
    $font=[Drawing.Font]::new($FontName,12,[Drawing.FontStyle]::Regular,[Drawing.GraphicsUnit]::Pixel)
    $textBrush=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255,$TextDelta,0,0))
    $alphaBrush=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb($(if($AlphaError){129}else{128}),200,30,70))
    try{
        $graphics.Clear([Drawing.Color]::White)
        if($Clip){$graphics.SetClip([Drawing.Rectangle]::new(0,0,20,64))}
        if(-not $Missing){$graphics.FillRectangle($brush,12+$Shift,10,30,20)}
        $graphics.ResetClip()
        if(-not $WrongOrder){$graphics.FillRectangle($alphaBrush,30,15,30,15)}
        $graphics.DrawString($(if($Wrap){"Aa`n123"}else{'Aa 123'}),$font,$textBrush,8+$TextShift,36)
        if($WrongOrder){$graphics.FillRectangle($brush,12+$Shift,10,30,20);$graphics.FillRectangle($alphaBrush,30,15,30,15);$graphics.FillRectangle($brush,12+$Shift,10,30,20)}
        if($TextRegionError){$graphics.FillRectangle([Drawing.Brushes]::Red,50,48,1,1)}
        $bitmap.Save($Path,[Drawing.Imaging.ImageFormat]::Png)
    }finally{$alphaBrush.Dispose();$textBrush.Dispose();$font.Dispose();$brush.Dispose();$graphics.Dispose();$bitmap.Dispose()}
}
$reference=Join-Path $OutputPath 'reference.png';Picture $reference
$tests=@(
    @{name='identical';expectedPass=$true},
    @{name='one-device-pixel-shift';expectedPass=$false;shift=1},
    @{name='one-channel-change';expectedPass=$false;delta=1},
    @{name='missing-element';expectedPass=$false;missing=$true},
    @{name='incorrect-clip';expectedPass=$false;clip=$true},
    @{name='wrong-font';expectedPass=$false;font='Courier New'},
    @{name='one-device-pixel-text-shift';expectedPass=$false;textShift=1},
    @{name='text-color-one-channel';expectedPass=$false;textDelta=1},
    @{name='wrong-line-break';expectedPass=$false;wrap=$true},
    @{name='nontext-error-inside-text-region';expectedPass=$false;textRegionError=$true},
    @{name='incorrect-alpha-composite';expectedPass=$false;alphaError=$true},
    @{name='wrong-stacking-order';expectedPass=$false;wrongOrder=$true}
)
$results=@()
foreach($test in $tests){
    $actual=Join-Path $OutputPath ($test.name+'.png')
    Picture -Path $actual -Shift ([int]$test.shift) -Delta ([int]$test.delta) -Missing:([bool]$test.missing) -Clip:([bool]$test.clip) -FontName $(if($test.font){$test.font}else{'Arial'}) -TextShift ([int]$test.textShift) -TextDelta ([int]$test.textDelta) -Wrap:([bool]$test.wrap) -TextRegionError:([bool]$test.textRegionError) -AlphaError:([bool]$test.alphaError) -WrongOrder:([bool]$test.wrongOrder)
    $diff=[StrictRenderingPixels]::Compare($reference,$actual,(Join-Path $OutputPath ($test.name+'-diff.png')))
    $passed=($diff.DifferentPixels -eq 0)
    if($passed -ne $test.expectedPass){throw ('Pixel comparator failed control: '+$test.name)}
    $results+=@{name=$test.name;expectedPass=$test.expectedPass;actualPass=$passed;differentPixels=$diff.DifferentPixels;controlVerified=$true}
}
$wrongSize=Join-Path $OutputPath 'wrong-size.png';$bitmap=[Drawing.Bitmap]::new(144,96)
try{$bitmap.Save($wrongSize,[Drawing.Imaging.ImageFormat]::Png)}finally{$bitmap.Dispose()}
$sizeRejected=$false
try{[StrictRenderingPixels]::Compare($reference,$wrongSize,'')|Out-Null}catch{$sizeRejected=$true}
if(-not $sizeRejected){throw 'Comparator accepted wrong DPI/image dimensions'}
$results+=@{name='wrong-dpi-dimensions';controlVerified=$true;actualPass=$false;expectedPass=$false}
$textReference=@([pscustomobject]@{path='0/1/0';start=0;length=1;text='A';mapped=$true;rect=@(8.0,12.0,9.0,17.0);glyph=@{collected=$true;fontResolved=$true;family='Arial';postScript='ArialMT';index=36}})
foreach($name in @('text-identical','text-one-device-pixel-shift','text-wrong-advance','text-wrong-line','text-missing-character','text-wrong-source-offset','text-wrong-character','text-unmapped-character','text-duplicate-range','text-null-coordinate','text-nan-coordinate','text-infinite-coordinate','text-missing-glyph','text-uncollected-glyph','text-unresolved-font')){
    $textNative=$textReference|ConvertTo-Json -Depth 5|ConvertFrom-Json
    switch($name){
        'text-one-device-pixel-shift' {$textNative.rect[0]+=1.0/1.5}
        'text-wrong-advance' {$textNative.rect[2]+=1}
        'text-wrong-line' {$textNative.rect[1]+=22}
        'text-missing-character' {$textNative=@()}
        'text-wrong-source-offset' {$textNative.start=1}
        'text-wrong-character' {$textNative.text='B'}
        'text-unmapped-character' {$textNative.mapped=$false}
        'text-duplicate-range' {$textNative=@($textNative,$textNative)}
        'text-null-coordinate' {$textNative.rect[0]=$null}
        'text-nan-coordinate' {$textNative.rect[0]=[double]::NaN}
        'text-infinite-coordinate' {$textNative.rect[0]=[double]::PositiveInfinity}
        'text-missing-glyph' {$textNative.glyph.index=0}
        'text-uncollected-glyph' {$textNative.glyph.collected=$false}
        'text-unresolved-font' {$textNative.glyph.fontResolved=$false}
    }
    $comparison=Compare-RenderingText $textReference @($textNative)
    $expected=($name -eq 'text-identical')
    if($comparison.passed -ne $expected){throw ('Text comparator failed control: '+$name)}
    $results+=@{name=$name;controlVerified=$true;actualPass=$comparison.passed;expectedPass=$expected;differenceCount=$comparison.differences.Count}
}
# A recorded backend AA sample is accepted; changed colors/positions and
# unrelated pixels must still fail, even when inside a text rectangle.
$backendNative=Join-Path $OutputPath 'backend-aa.png'
$bitmap=[Drawing.Bitmap]::new($reference)
try{
    $changed=$false
    for($y=36;$y -lt 64 -and -not $changed;$y++){for($x=8;$x -lt 88;$x++){
        $color=$bitmap.GetPixel($x,$y)
        if($color.R -gt 8 -and $color.R -lt 247){
            $bitmap.SetPixel($x,$y,[Drawing.Color]::FromArgb($color.A,$color.R+1,$color.G,$color.B));$changed=$true;break
        }
    }}
    if(-not $changed){throw 'No font AA sample found in calibration image'}
    $bitmap.Save($backendNative,[Drawing.Imaging.ImageFormat]::Png)
}finally{$bitmap.Dispose()}
$backendPixel=[StrictRenderingPixels]::Compare($reference,$backendNative,'')
$backendPolicy=Join-Path $OutputPath 'backend-policy.json'
$entry=@{caseId='backend-control';dpi=96;viewport='96x64';kind='CPU_GPU_FONT_COMPOSITION';
    referencePixelSha256=[StrictRenderingPixels]::PixelSha256($reference);
    nativePixelSha256=[StrictRenderingPixels]::PixelSha256($backendNative);
    differentPixels=$backendPixel.DifferentPixels;maximumChannelDelta=$backendPixel.MaximumChannelDelta}
$rasterEntry=$entry.Clone();$rasterEntry.caseId='raster-control';$rasterEntry.kind='CPU_GPU_RASTER_ANTIALIASING'
@{schemaVersion=1;exceptions=@($entry,$rasterEntry)}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $backendPolicy -Encoding utf8
$reencoded=Join-Path $OutputPath 'backend-aa-reencoded.png'
# Add valid ancillary metadata without resampling or color-converting the image.
$png=[IO.File]::ReadAllBytes($backendNative)
$chunkType=[Text.Encoding]::ASCII.GetBytes('tEXt')
$chunkData=[Text.Encoding]::ASCII.GetBytes("Validation`0encoding independent")
$crc=[uint32]::MaxValue
foreach($value in ($chunkType+$chunkData)){
    $crc=$crc -bxor [uint32]$value
    for($bit=0;$bit -lt 8;$bit++){
        $crc=if($crc -band 1){($crc -shr 1) -bxor [uint32]3988292384}else{$crc -shr 1}
    }
}
$chunkLength=[BitConverter]::GetBytes([int]$chunkData.Length);[Array]::Reverse($chunkLength)
$chunkCrc=[BitConverter]::GetBytes([uint32]($crc -bxor [uint32]::MaxValue));[Array]::Reverse($chunkCrc)
$stream=[IO.MemoryStream]::new()
try{
    $stream.Write($png,0,$png.Length-12)
    foreach($bytes in @($chunkLength,$chunkType,$chunkData,$chunkCrc)){$stream.Write($bytes,0,$bytes.Length)}
    $stream.Write($png,$png.Length-12,12)
    [IO.File]::WriteAllBytes($reencoded,$stream.ToArray())
}finally{$stream.Dispose()}
foreach($test in @(
    @{name='approved-backend-aa';path=$backendNative;id='backend-control';dpi=96;expected=$true},
    @{name='backend-decoded-pixels';path=$reencoded;id='backend-control';dpi=96;expected=$true},
    @{name='backend-unrelated-pixel';path=(Join-Path $OutputPath 'nontext-error-inside-text-region.png');id='backend-control';dpi=96;expected=$false},
    @{name='backend-wrong-text-color';path=(Join-Path $OutputPath 'text-color-one-channel.png');id='backend-control';dpi=96;expected=$false},
    @{name='backend-wrong-case';path=$backendNative;id='other-control';dpi=96;expected=$false},
    @{name='backend-wrong-dpi';path=$backendNative;id='backend-control';dpi=144;expected=$false}
    @{name='approved-raster-aa';path=$backendNative;id='raster-control';dpi=96;expected=$true},
    @{name='raster-aa-unrelated-pixel';path=(Join-Path $OutputPath 'nontext-error-inside-text-region.png');id='raster-control';dpi=96;expected=$false}
)){
    $pixel=[StrictRenderingPixels]::Compare($reference,$test.path,'')
    $approval=Find-RenderingBackendException -ReferencePath $reference -NativePath $test.path -CaseId $test.id -Dpi $test.dpi -Viewport '96x64' -Pixel $pixel -RegistryPath $backendPolicy
    $passed=$null -ne $approval
    if($passed -ne $test.expected){throw ('Backend exception failed control: '+$test.name)}
    $results+=@{name=$test.name;controlVerified=$true;actualPass=$passed;expectedPass=$test.expected}
}
foreach($controlId in @('backend-control','raster-control')){foreach($wrongGeometry in @($false,$true)){
    $folder=Join-Path $OutputPath ($(if($wrongGeometry){'backend-wrong-geometry'}else{'backend-approved-capture'})+'\'+$controlId)
    [IO.Directory]::CreateDirectory($folder)|Out-Null
    [IO.File]::Copy($reference,(Join-Path $folder 'reference.png'));[IO.File]::Copy($backendNative,(Join-Path $folder 'native.png'))
    @{status='CAPTURED';runtime='calibration'}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $folder 'capture-status.json')
    @{width=96;height=64;dpr=1}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $folder 'reference-metrics.json')
    @{dom=@();boxes=@();textGeometry=@($textReference)}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $folder 'reference.json')
    $nativeChar=$textReference|ConvertTo-Json -Depth 8|ConvertFrom-Json
    if($wrongGeometry){$nativeChar.rect[0]+=1}
    @{width=96;height=64;dpi=96;windowDpi=96;captureRoute='calibration';dom=@();boxes=@();textGeometry=@($nativeChar)}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $folder 'native.json')
    $comparison=& (Join-Path $PSScriptRoot 'Compare-RenderingCapture.ps1') -CapturePath $folder -BackendExceptionsPath $backendPolicy
    $expected=-not $wrongGeometry
    if($comparison.pass -ne $expected -or ($expected -and $comparison.status -ne 'PASS_BACKEND_DIFFERENCE')){throw 'Backend exception bypassed structural checks'}
    $results+=@{name=($controlId+'-'+$(if($wrongGeometry){'backend-wrong-geometry'}else{'backend-approved-capture'}));controlVerified=$true;actualPass=$comparison.pass;expectedPass=$expected}
}}
# Exercise the capture gate, including a native image whose backend difference
# is approved. That approval must never excuse a bad reference capture route.
foreach($test in @(
    @{name='cdp-identical';cdp=$reference;expected=$true},
    @{name='cdp-png-metadata';reference=$backendNative;native=$backendNative;cdp=$reencoded;expected=$true},
    @{name='cdp-wrong-pixel';cdp=(Join-Path $OutputPath 'one-channel-change.png');expected=$false},
    @{name='cdp-wrong-dimensions';cdp=$wrongSize;expected=$false},
    @{name='cdp-missing';expected=$false},
    @{name='cdp-corrupt';corrupt=$true;expected=$false},
    @{name='cdp-backend-approval-does-not-bypass';native=$backendNative;cdp=$backendNative;expected=$false}
)){
    $folder=Join-Path $OutputPath ($test.name+'\backend-control')
    [IO.Directory]::CreateDirectory($folder)|Out-Null
    [IO.File]::Copy($(if($test.reference){$test.reference}else{$reference}),(Join-Path $folder 'reference.png'))
    [IO.File]::Copy($(if($test.native){$test.native}else{$reference}),(Join-Path $folder 'native.png'))
    if($test.cdp){[IO.File]::Copy($test.cdp,(Join-Path $folder 'reference-cdp.png'))}
    if($test.corrupt){[IO.File]::WriteAllText((Join-Path $folder 'reference-cdp.png'),'invalid PNG')}
    @{status='CAPTURED';captureSchemaVersion=3;runtime='calibration'}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $folder 'capture-status.json')
    @{width=96;height=64;dpr=1}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $folder 'reference-metrics.json')
    @{dom=@();boxes=@();textGeometry=@()}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $folder 'reference.json')
    @{width=96;height=64;dpi=96;windowDpi=96;captureRoute='calibration';dom=@();boxes=@();textGeometry=@()}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $folder 'native.json')
    $comparison=& (Join-Path $PSScriptRoot 'Compare-RenderingCapture.ps1') -CapturePath $folder -BackendExceptionsPath $backendPolicy
    if($comparison.pass -ne $test.expected -or ($test.expected -and -not $comparison.referenceRoute.matches) -or (-not $test.expected -and 'HARNESS_ERROR' -notin $comparison.failures.kind)){throw ('Capture route comparator failed control: '+$test.name)}
    $results+=@{name=$test.name;controlVerified=$true;actualPass=$comparison.pass;expectedPass=$test.expected}
}
$results+=@(& (Join-Path $PSScriptRoot 'Test-RenderingStyleControls.ps1') -OutputPath (Join-Path $OutputPath 'styles') -ReferenceImage $reference -BackendImage $backendNative -BackendPolicy $backendPolicy)
[IO.File]::WriteAllText((Join-Path $OutputPath 'controls.json'),(ConvertTo-Json -InputObject $results -Depth 8),[Text.UTF8Encoding]::new($false))
Write-Output ('Verified '+$results.Count+' comparison controls: '+$OutputPath)
