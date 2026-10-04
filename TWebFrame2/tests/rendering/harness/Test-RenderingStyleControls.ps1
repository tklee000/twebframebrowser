param([Parameter(Mandatory=$true)][string]$OutputPath,[Parameter(Mandatory=$true)][string]$ReferenceImage,[Parameter(Mandatory=$true)][string]$BackendImage,[Parameter(Mandatory=$true)][string]$BackendPolicy)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Compare-RenderingStyles.ps1')
if(Test-Path -LiteralPath $OutputPath){throw 'Style controls already exist'}
[IO.Directory]::CreateDirectory($OutputPath)|Out-Null
$base=[ordered]@{'display'='block';'position'='static';'visibility'='visible';'box-sizing'='content-box';'white-space'='normal';'direction'='ltr';'text-align'='start';'font-style'='normal';'font-weight'='400';'font-kerning'='auto';'pointer-events'='auto';'overflow-x'='visible';'overflow-y'='visible';'flex-direction'='row';'flex-wrap'='nowrap';'flex-grow'='0';'flex-shrink'='1';'list-style-position'='outside';'list-style-type'='disc';'opacity'='1';'letter-spacing'='normal';'line-height'='normal';'font-size'='16px';'color'='rgb(23, 32, 42)';'background-color'='rgba(0, 0, 0, 0)'}
$wrong=@{'display'='inline';'position'='relative';'visibility'='hidden';'box-sizing'='border-box';'white-space'='pre';'direction'='rtl';'text-align'='center';'font-style'='italic';'font-weight'='700';'font-kerning'='none';'pointer-events'='none';'overflow-x'='hidden';'overflow-y'='auto';'flex-direction'='column';'flex-wrap'='wrap';'flex-grow'='1';'flex-shrink'='0';'list-style-position'='inside';'list-style-type'='decimal';'opacity'='.5';'letter-spacing'='1px';'line-height'='20px';'font-size'='16.02px';'color'='rgb(24, 32, 42)';'background-color'='rgba(0, 0, 0, .1)'}
function Copy-Styles { $base|ConvertTo-Json|ConvertFrom-Json }
$results=@()
foreach($property in $base.Keys){foreach($mode in @('wrong','missing-native','missing-reference')){
    $r=Copy-Styles;$n=Copy-Styles
    if($mode -eq 'wrong'){$n.$property=$wrong[$property]}
    elseif($mode -eq 'missing-native'){$n.PSObject.Properties.Remove($property)}
    else{$r.PSObject.Properties.Remove($property)}
    $comparison=Compare-RenderingStyles $r $n
    if($comparison.passed -or $property -notin $comparison.differences.property){throw ('Style error was not detected: '+$mode+' '+$property)}
    $results+=@{name=('style-'+$mode+'-'+$property);controlVerified=$true;actualPass=$false;expectedPass=$false}
}}
foreach($test in @('equivalent-serialization','rtl-logical-alignment','finite-decimal-length','alpha-quantization','invalid-number','invalid-color','empty-value')){
    $r=Copy-Styles;$n=Copy-Styles;$expected=$true
    switch($test){
        'equivalent-serialization' {$n.'font-weight'='normal';$n.color='#17202aff';$n.'background-color'='#00000000';$n.opacity='1.0000';$n.'text-align'='left'}
        'rtl-logical-alignment' {$r.direction='rtl';$n.direction='rtl';$n.'text-align'='right'}
        'finite-decimal-length' {$r.'font-size'='13.3333px';$n.'font-size'='13.3332996px'}
        'alpha-quantization' {$r.'background-color'='rgba(20, 40, 60, 0.5)';$n.'background-color'='#14283c80'}
        'invalid-number' {$n.opacity='NaN';$expected=$false}
        'invalid-color' {$n.color='rgb(999, 0, 0)';$expected=$false}
        'empty-value' {$n.'white-space'='';$expected=$false}
    }
    $comparison=Compare-RenderingStyles $r $n
    if($comparison.passed -ne $expected){throw ('Style normalization control failed: '+$test)}
    $results+=@{name=('style-'+$test);controlVerified=$true;actualPass=$comparison.passed;expectedPass=$expected}
}
foreach($test in @('missing-contract','missing-projection','hidden-wrong-style','backend-wrong-style','complete-contract')){
    $folder=Join-Path $OutputPath ($test+'\backend-control');[IO.Directory]::CreateDirectory($folder)|Out-Null
    foreach($image in @('reference.png','reference-cdp.png')){[IO.File]::Copy($ReferenceImage,(Join-Path $folder $image))}
    [IO.File]::Copy($(if($test -eq 'backend-wrong-style'){$BackendImage}else{$ReferenceImage}),(Join-Path $folder 'native.png'))
    @{status='CAPTURED';captureSchemaVersion=4;runtime='calibration'}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $folder 'capture-status.json')
    @{width=96;height=64;dpr=1}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $folder 'reference-metrics.json')
    $rStyles=Copy-Styles;$nStyles=Copy-Styles
    $present=$test -ne 'hidden-wrong-style'
    if($test -in @('hidden-wrong-style','backend-wrong-style')){$nStyles.color='rgb(24, 32, 42)'}
    if($test -eq 'missing-projection'){$nStyles=$null}
    $r=@{styleContractVersion=1;dom=@();boxes=@(@{id='box';present=$present;rect=@(0,0,10,10);computedStyles=$rStyles});textGeometry=@()}
    $n=@{styleContractVersion=1;width=96;height=64;dpi=96;windowDpi=96;dom=@();boxes=@(@{id='box';present=$present;rect=@(0,0,10,10);computedStyles=$nStyles});textGeometry=@()}
    if($test -eq 'missing-contract'){$n.Remove('styleContractVersion');$r.Remove('styleContractVersion')}
    $r|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $folder 'reference.json')
    $n|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $folder 'native.json')
    $comparison=& (Join-Path $PSScriptRoot 'Compare-RenderingCapture.ps1') -CapturePath $folder -BackendExceptionsPath $BackendPolicy
    $expected=$test -eq 'complete-contract'
    if($comparison.pass -ne $expected -or ($test -eq 'missing-contract' -and 'HARNESS_ERROR' -notin $comparison.failures.kind) -or ($test -in @('missing-projection','hidden-wrong-style','backend-wrong-style') -and 'FAIL_STYLE' -notin $comparison.failures.kind)){throw ('Style capture gate failed: '+$test)}
    $results+=@{name=('style-capture-'+$test);controlVerified=$true;actualPass=$comparison.pass;expectedPass=$expected}
}
[IO.File]::WriteAllText((Join-Path $OutputPath 'controls.json'),(ConvertTo-Json -InputObject $results -Depth 8),[Text.UTF8Encoding]::new($false))
$results
