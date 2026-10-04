param([Parameter(Mandatory=$true)][string]$CapturePath,[string]$ComparisonPath='', [switch]$StrictPixels,
      [string]$BackendExceptionsPath=(Join-Path $PSScriptRoot '../backend-pixel-exceptions.json'))
$ErrorActionPreference='Stop'
if(-not $ComparisonPath){$ComparisonPath=$CapturePath}
if(Test-Path -LiteralPath (Join-Path $ComparisonPath 'result.json')){throw 'Comparison already exists; select a new ComparisonPath to preserve it'}
[IO.Directory]::CreateDirectory($ComparisonPath)|Out-Null
. (Join-Path $PSScriptRoot 'Import-PixelComparison.ps1')
. (Join-Path $PSScriptRoot 'Compare-RenderingText.ps1')
. (Join-Path $PSScriptRoot 'Compare-RenderingBackend.ps1')
. (Join-Path $PSScriptRoot 'Compare-RenderingStyles.ps1')
function Get-StyleValue($Styles,[string]$Name){$p=$Styles.PSObject.Properties[$Name];if($null -eq $p){return ''};return [string]$p.Value}
function Normalize-Style([string]$Name,[string]$Value){
    $v=$Value.Trim().ToLowerInvariant()
    if($Name -eq 'font-weight'){if($v -eq 'normal'){$v='400'};if($v -eq 'bold'){$v='700'}}
    return $v
}
$status=Get-Content -LiteralPath (Join-Path $CapturePath 'capture-status.json') -Raw -Encoding UTF8|ConvertFrom-Json
$failures=[Collections.Generic.List[object]]::new()
$layoutFailures=[Collections.Generic.List[object]]::new()
$textComparison=$null
$styleDifferences=[Collections.Generic.List[object]]::new()
$styleChecked=0;$styleMissing=0;$legacyStyleSkipped=0
if($status.status -ne 'CAPTURED'){
    $result=[ordered]@{schemaVersion=1;status=$status.status;message=$status.message;pass=$false;fullContractComplete=$false}
}else{
    $reference=Get-Content -LiteralPath (Join-Path $CapturePath 'reference.json') -Raw -Encoding UTF8|ConvertFrom-Json
    $native=Get-Content -LiteralPath (Join-Path $CapturePath 'native.json') -Raw -Encoding UTF8|ConvertFrom-Json
    $metrics=Get-Content -LiteralPath (Join-Path $CapturePath 'reference-metrics.json') -Raw -Encoding UTF8|ConvertFrom-Json
    $requiredStyles=$status.captureSchemaVersion -ge 4 -or $null -ne $native.styleContractVersion -or $null -ne $reference.styleContractVersion
    if($requiredStyles -and ($native.styleContractVersion -ne 1 -or $reference.styleContractVersion -ne 1)){
        $failures.Add(@{kind='HARNESS_ERROR';message='Required computed style contract version is missing or unsupported'})
    }
    if($metrics.width -ne $native.width -or $metrics.height -ne $native.height -or [Math]::Abs($metrics.dpr-$native.dpi/96.0) -gt .00001){$failures.Add(@{kind='HARNESS_ERROR';message='Viewport/DPR mismatch';metrics=$metrics})}
    $referenceRoute=[ordered]@{tested=$false;reason='Legacy capture without CDP screenshot'}
    $cdpFile=Join-Path $CapturePath 'reference-cdp.png'
    if(Test-Path -LiteralPath $cdpFile){
        try{
            $calibration=[StrictRenderingPixels]::Compare((Join-Path $CapturePath 'reference.png'),$cdpFile,(Join-Path $ComparisonPath 'reference-route-diff.png'))
            $referenceRoute=[ordered]@{tested=$true;routes=@('CapturePreview','Page.captureScreenshot');matches=($calibration.DifferentPixels -eq 0);differentPixels=$calibration.DifferentPixels;maximumChannelDelta=$calibration.MaximumChannelDelta}
            if($calibration.DifferentPixels){$failures.Add(@{kind='HARNESS_ERROR';message='WebView2 capture routes differ';differentPixels=$calibration.DifferentPixels})}
        }catch{$failures.Add(@{kind='HARNESS_ERROR';message=('WebView2 capture calibration failed: '+$_.Exception.Message)})}
    }elseif($status.captureSchemaVersion -ge 3){$failures.Add(@{kind='HARNESS_ERROR';message='Required CDP screenshot is missing'})}
    [IO.File]::WriteAllText((Join-Path $ComparisonPath 'reference-route.json'),($referenceRoute|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
    $windowRouteFile=Join-Path $CapturePath 'window-route.json'
    $windowRoute=$null
    if(Test-Path -LiteralPath $windowRouteFile){
        $windowRoute=Get-Content -LiteralPath $windowRouteFile -Raw -Encoding UTF8|ConvertFrom-Json
        if($windowRoute.tested -and -not $windowRoute.matchesExplicit){$failures.Add(@{kind='FAIL_VIEW_ROUTE';message='Window paint differs from explicit-DPI paint';windowDpi=$windowRoute.windowDpi})}
    }
    # Compare authored DOM, including text/attributes and parser-created nodes.
    if($reference.dom.Count -ne $native.dom.Count){$failures.Add(@{kind='FAIL_DOM';message='Node count differs';reference=$reference.dom.Count;native=$native.dom.Count})}
    $nodeCount=[Math]::Min($reference.dom.Count,$native.dom.Count)
    for($i=0;$i -lt $nodeCount;$i++){
        $r=$reference.dom[$i];$n=$native.dom[$i]
        $rAttrs=($r.attrs.PSObject.Properties|Sort-Object Name|ForEach-Object{$_.Name+'='+[string]$_.Value}) -join "`n"
        $nAttrs=($n.attrs.PSObject.Properties|Sort-Object Name|ForEach-Object{$_.Name+'='+[string]$_.Value}) -join "`n"
        if($r.type -ne $n.type -or $r.path -cne $n.path -or ($r.type -eq 1 -and $r.tag -cne $n.tag) -or $r.text -cne $n.text -or $rAttrs -cne $nAttrs){
            $failures.Add(@{kind='FAIL_DOM';index=$i;reference=$r;native=$n})
        }
    }
    $nativeBoxes=@{};foreach($box in $native.boxes){
        if($nativeBoxes.ContainsKey($box.id)){$failures.Add(@{kind='HARNESS_ERROR';message='Duplicate native box ID';id=$box.id})}
        $nativeBoxes[$box.id]=$box
    }
    $referenceIds=@{}
    foreach($r in $reference.boxes){
        if($referenceIds.ContainsKey($r.id)){$failures.Add(@{kind='HARNESS_ERROR';message='Duplicate reference box ID';id=$r.id})}
        $referenceIds[$r.id]=$true
        $n=$nativeBoxes[$r.id]
        if($null -eq $n -or $r.present -ne $n.present){$layoutFailures.Add(@{id=$r.id;message='Box presence differs';reference=$r;native=$n});continue}
        if($requiredStyles){
            $styleComparison=Compare-RenderingStyles $r.computedStyles $n.computedStyles
            $styleChecked+=$styleComparison.checked;$styleMissing+=$styleComparison.missing
            foreach($difference in $styleComparison.differences){
                $difference.id=$r.id;$styleDifferences.Add($difference)
                $failures.Add(@{kind='FAIL_STYLE';id=$r.id;property=$difference.property;message=$difference.message;reference=$difference.reference;native=$difference.native})
            }
        }
        if($r.present){
            for($axis=0;$axis -lt 4;$axis++){
                if([Math]::Abs($r.rect[$axis]-$n.rect[$axis]) -gt (1.0/64.0)){$layoutFailures.Add(@{id=$r.id;axis=$axis;reference=$r.rect[$axis];native=$n.rect[$axis]})}
            }
            if(-not $requiredStyles){foreach($property in @('display','position','visibility','box-sizing','white-space','direction','text-align','font-style','font-weight','font-kerning')){
                $rValue=Normalize-Style $property (Get-StyleValue $r.styles $property)
                $nValue=Normalize-Style $property (Get-StyleValue $n.styles $property)
                if($property -eq 'text-align'){
                    $rDirection=Get-StyleValue $r.styles 'direction';$nDirection=Get-StyleValue $n.styles 'direction'
                    if($rValue -eq 'start'){$rValue=if($rDirection -eq 'rtl'){'right'}else{'left'}}
                    if($rValue -eq 'end'){$rValue=if($rDirection -eq 'rtl'){'left'}else{'right'}}
                    if($nValue -eq 'start'){$nValue=if($nDirection -eq 'rtl'){'right'}else{'left'}}
                    if($nValue -eq 'end'){$nValue=if($nDirection -eq 'rtl'){'left'}else{'right'}}
                }
                if($nValue -eq ''){$legacyStyleSkipped++;continue} # Legacy coverage is reported explicitly, never upgraded.
                if($rValue -cne $nValue){$failures.Add(@{kind='FAIL_STYLE';id=$r.id;property=$property;reference=$rValue;native=$nValue})}
            }}
        }
    }
    foreach($id in $nativeBoxes.Keys){if(-not $referenceIds.ContainsKey($id)){$layoutFailures.Add(@{id=$id;message='Unexpected native box'})}}
    if($layoutFailures.Count){$failures.Add(@{kind='FAIL_LAYOUT';count=$layoutFailures.Count})}
    $hasReferenceText=$null -ne $reference.PSObject.Properties['textGeometry']
    $hasNativeText=$null -ne $native.PSObject.Properties['textGeometry']
    if($hasReferenceText -and $hasNativeText){
        $textComparison=Compare-RenderingText $reference.textGeometry $native.textGeometry
        if(-not $textComparison.passed){$failures.Add(@{kind='FAIL_TEXT';count=$textComparison.differences.Count})}
    }elseif($hasReferenceText -ne $hasNativeText){
        $failures.Add(@{kind='HARNESS_ERROR';message='Text measurement is missing from one engine'})
    }
    $pixel=[StrictRenderingPixels]::Compare((Join-Path $CapturePath 'reference.png'),(Join-Path $CapturePath 'native.png'),(Join-Path $ComparisonPath 'diff.png'))
    $strictPass = $failures.Count -eq 0 -and $pixel.DifferentPixels -eq 0
    $backendException = $null
    if (-not $StrictPixels -and -not $failures.Count -and $pixel.DifferentPixels) {
        $backendException = Find-RenderingBackendException -ReferencePath (Join-Path $CapturePath 'reference.png') `
            -NativePath (Join-Path $CapturePath 'native.png') -CaseId (Split-Path $CapturePath -Leaf) `
            -Dpi $native.dpi -Viewport ($native.width.ToString()+'x'+$native.height) -Pixel $pixel -RegistryPath $BackendExceptionsPath
    }
    $allowedPixels = if ($null -ne $backendException) { $pixel.DifferentPixels } else { 0 }
    if($pixel.DifferentPixels -gt $allowedPixels){$failures.Add(@{kind='FAIL_PAINT';pixels=$pixel.DifferentPixels-$allowedPixels})}
    $result=[ordered]@{schemaVersion=1;status=$(if($failures.Count){'FAIL'}elseif($allowedPixels){'PASS_BACKEND_DIFFERENCE'}else{'PASS'});pass=($failures.Count -eq 0);strictPass=$strictPass;strictStatus=$(if($strictPass){'PASS'}else{'FAIL'});capturePath=$CapturePath;captureRoute=$native.captureRoute;windowDpi=$native.windowDpi;windowRoute=$windowRoute;dpi=$native.dpi;viewport=@($native.width,$native.height);runtime=$status.runtime;differentPixels=$pixel.DifferentPixels;allowedFontAAPixels=$(if($backendException.kind -eq 'CPU_GPU_FONT_COMPOSITION'){$allowedPixels}else{0});allowedBackendDifferencePixels=$allowedPixels;disallowedDifferentPixels=$pixel.DifferentPixels-$allowedPixels;maximumChannelDelta=$pixel.MaximumChannelDelta;differenceBounds=@($pixel.Left,$pixel.Top,$pixel.Right,$pixel.Bottom);backendException=$backendException;failures=@($failures.ToArray());styleCoverage='initial semantic subset; all raw styles preserved';fontAAPolicy=$(if($StrictPixels){'strict pixel equality; backend exceptions disabled'}else{'exact pixels by default; approved CPU/GPU font, gradient and raster AA pixel signatures excluded'});textGeometryCoverage='native text fragments and CDP text boxes preserved; full matching pending';fullContractComplete=$false}
    $result.referenceRoute=$referenceRoute
    $result.styleCoverage=[ordered]@{contractVersion=$(if($requiredStyles){1}else{0});properties=$(if($requiredStyles){@(Get-RenderingStyleProperties)}else{@('display','position','visibility','box-sizing','white-space','direction','text-align','font-style','font-weight','font-kerning')});checked=$styleChecked;missing=$styleMissing;legacySkipped=$legacyStyleSkipped;includesNonRenderedElements=[bool]$requiredStyles;remainingComputedStylesPending=$true}
}
$encoding=[Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $ComparisonPath 'layout-diff.json'),(ConvertTo-Json -InputObject @($layoutFailures.ToArray()) -Depth 12),$encoding)
[IO.File]::WriteAllText((Join-Path $ComparisonPath 'style-diff.json'),(ConvertTo-Json -InputObject @($styleDifferences.ToArray()) -Depth 12),$encoding)
if($null -ne $textComparison){
    $result.textGeometryCoverage='UTF-16 non-space character source ranges and font-box rects; shaped native glyph indices, advances and resolved fonts when present; CDP node font aggregates preserved separately; reference per-character baselines pending'
    $result.textGeometry=$textComparison | Select-Object passed,referenceCharacters,nativeCharacters
    [IO.File]::WriteAllText((Join-Path $ComparisonPath 'text-diff.json'),(ConvertTo-Json -InputObject $textComparison.differences -Depth 12),$encoding)
}
[IO.File]::WriteAllText((Join-Path $ComparisonPath 'result.json'),($result|ConvertTo-Json -Depth 18),$encoding)
[pscustomobject]$result
