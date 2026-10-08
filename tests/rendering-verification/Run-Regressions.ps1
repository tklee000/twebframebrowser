param([string]$BuildDirectory=(Join-Path $PSScriptRoot '.work\build'),[string]$PythonPath='',[switch]$SkipEngineBuild)
$ErrorActionPreference='Stop'
$buildFull=[IO.Path]::GetFullPath($BuildDirectory)
if(!$SkipEngineBuild){& (Join-Path $PSScriptRoot 'Build-Capture.ps1') -BuildDirectory $buildFull}
$msbuild='C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe'
$failedChecks=[Collections.Generic.List[string]]::new()
foreach($project in @('FontLogicalRegression','ViewportDpiRegression','AutoOverflowRegression','TableFormattingRegression','OverflowPaddingRegression','TextSpacingRegression','AutoTableRegression','TextWrappingRegression','TableHeightRegression','IntrinsicWidthRegression','TextMinimumRegression','FlexSizingRegression','DirectionAttributeRegression','ItemOrderRegression','ClipPathRegression','OpacityGroupRegression','GridAutoFlowRegression','MixedInlineFlowRegression','MulticolBlockFlowRegression')){
    & $msbuild (Join-Path $PSScriptRoot ($project+'.vcxproj')) /t:Build /p:Configuration=Release /p:Platform=x64 /p:BuildProjectReferences=false "/p:OutDir=$buildFull\bin\" "/p:IntDir=$buildFull\obj\$project\" /v:minimal /nologo
    if($LASTEXITCODE -ne 0){throw "Regression build failed: $project"}
    if($project -eq 'MulticolBlockFlowRegression'){
        & (Join-Path $buildFull ('bin\'+$project+'.exe')) (Join-Path $PSScriptRoot 'regressions\multicol-block-flow')
    }elseif($project -eq 'MixedInlineFlowRegression'){
        & (Join-Path $buildFull ('bin\'+$project+'.exe')) (Join-Path $PSScriptRoot 'regressions\mixed-inline-flow')
    }elseif($project -eq 'GridAutoFlowRegression'){
        & (Join-Path $buildFull ('bin\'+$project+'.exe')) (Join-Path $PSScriptRoot 'regressions\grid-auto-flow')
    }elseif($project -eq 'ClipPathRegression'){
        & (Join-Path $buildFull ('bin\'+$project+'.exe')) (Join-Path $PSScriptRoot 'regressions')
    }elseif($project -eq 'OpacityGroupRegression'){
        & (Join-Path $buildFull ('bin\'+$project+'.exe')) (Join-Path $PSScriptRoot 'regressions\opacity-group')
    }elseif($project -in @('AutoOverflowRegression','TableFormattingRegression','OverflowPaddingRegression','TextSpacingRegression','AutoTableRegression','TextWrappingRegression','TableHeightRegression','IntrinsicWidthRegression','TextMinimumRegression','FlexSizingRegression','DirectionAttributeRegression','ItemOrderRegression')){
        $regressionFixture=if($project -eq 'AutoOverflowRegression'){'auto-overflow'}elseif($project -eq 'TableFormattingRegression'){'table-formatting'}elseif($project -eq 'OverflowPaddingRegression'){'overflow-padding'}elseif($project -eq 'TextSpacingRegression'){'text-spacing'}elseif($project -eq 'AutoTableRegression'){'auto-table'}elseif($project -eq 'TextWrappingRegression'){'text-wrapping'}elseif($project -eq 'TableHeightRegression'){'table-height'}elseif($project -eq 'TextMinimumRegression'){'text-minimum'}elseif($project -eq 'FlexSizingRegression'){'flex-sizing'}elseif($project -eq 'DirectionAttributeRegression'){'direction-attribute'}elseif($project -eq 'ItemOrderRegression'){'item-order'}else{'intrinsic-width'}
        & (Join-Path $buildFull ('bin\'+$project+'.exe')) (Join-Path $PSScriptRoot ('regressions\'+$regressionFixture))
    }else{& (Join-Path $buildFull ('bin\'+$project+'.exe'))}
    if($LASTEXITCODE -ne 0){$failedChecks.Add($project);Write-Output ("Regression failed: $project; remaining checks continue.")}
}
if(!$PythonPath){$PythonPath=Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'}
if(!(Test-Path -LiteralPath $PythonPath)){throw 'Pass -PythonPath for a Python runtime with Pillow and NumPy.'}
& $PythonPath -m unittest discover -s $PSScriptRoot -p test_compare.py -v
if($LASTEXITCODE -ne 0){$failedChecks.Add('Python pixel comparator')}
try{& (Join-Path $PSScriptRoot 'Run-ExactPngRegressions.ps1') -BuildDirectory $buildFull -PythonPath $PythonPath}
catch{$failedChecks.Add('Exact PNG regression');Write-Output $_.Exception.Message}
if($failedChecks.Count){throw ('Regression failures: '+($failedChecks -join ', '))}
Write-Output 'Common CSS/layout/DPI and strict pixel comparator regressions passed.'
