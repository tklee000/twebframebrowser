param([switch]$FullRegression,
      [string]$MSBuild='C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe')
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$taskPrevious=Get-Location
try {
    Set-Location -LiteralPath $taskRoot
    New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot 'artifacts') -Force | Out-Null
    $taskProjects=@('SupportCompatibilityRegression')
    if($FullRegression){$taskProjects+=@('TWebFrameTests','CSSCompatibilityRegression','ScrollRenderingRegression','TableSpanRegression','FormControlRegression','RuntimeHeapRegression','BrowserContextRegression','StandaloneScriptRegression','ScriptHttpRegression','CanvasRegression','PointerEventRegression')}
    foreach($taskProject in $taskProjects){
        & $MSBuild (Join-Path $PSScriptRoot ($taskProject+'.vcxproj')) /t:Build /p:Configuration=Release /p:Platform=x64 (('/p:ForceImportBeforeCppTargets=')+(Join-Path $PSScriptRoot 'SupportBuild.props')) /m:2 /v:minimal /nologo
        if($LASTEXITCODE -ne 0){throw ('Build failed: '+$taskProject)}
        $taskExecutable=Join-Path $PSScriptRoot ('bin\x64\Release\'+$taskProject+'.exe')
        & $taskExecutable
        if($LASTEXITCODE -ne 0){throw ('Regression failed: '+$taskProject)}
        if($taskProject -eq 'TWebFrameTests'){
            & $taskExecutable --execution-regression (Join-Path $PSScriptRoot 'platform-integrity-regression.js')
            if($LASTEXITCODE -ne 0){throw 'Regression failed: platform integrity'}
        }
    }
} finally {Set-Location -LiteralPath $taskPrevious}
