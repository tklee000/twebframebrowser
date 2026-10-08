param([int]$Width=800,[int]$Height=600,[ValidateRange(1,6)][int]$NativeWorkers=3,[string]$PythonPath='',
      [string]$RunDirectory='', [switch]$CleanOnSuccess=$true)
$ErrorActionPreference='Stop'
if(!$PythonPath){$PythonPath=Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'}
if(!(Test-Path -LiteralPath $PythonPath)){throw 'Pass -PythonPath for a Python runtime with Pillow and NumPy.'}
if(!$RunDirectory){$RunDirectory=Join-Path $PSScriptRoot ('.work\runs\verification-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))}
$runFull=[IO.Path]::GetFullPath($RunDirectory)
if(Test-Path -LiteralPath $runFull){throw 'Use a new run directory.'}
$buildDirectory=Join-Path $PSScriptRoot '.work\build'
& (Join-Path $PSScriptRoot 'Build-Capture.ps1') -BuildDirectory $buildDirectory
if(!$?){throw 'Capture build failed.'}
$regressionFailure=''
try{
    & (Join-Path $PSScriptRoot 'Run-Regressions.ps1') -BuildDirectory $buildDirectory -PythonPath $PythonPath -SkipEngineBuild
}catch{
    # Still compare every original document when a small regression fails.
    # A regression failure remains an overall failure and prevents cleanup.
    $regressionFailure=$_.Exception.Message
    Write-Warning ("Regression failed; continuing the full document matrix: "+$regressionFailure)
}
[IO.Directory]::CreateDirectory($runFull) | Out-Null
foreach($mode in @('reference','software','native')){
    try{
        $options=@{Width=$Width;Height=$Height;Dpi=@(96,144);SkipBuild=$true;ParallelDpi=$true;BuildDirectory=$buildDirectory;OutputDirectory=(Join-Path $runFull $mode)}
        if($mode -eq 'native'){& (Join-Path $PSScriptRoot 'Capture-TWebFrame2.ps1') @options -Workers $NativeWorkers}
        else{& (Join-Path $PSScriptRoot 'Capture-WebView2.ps1') @options -Software:($mode -eq 'software')}
    }catch{
        # A missing reference PNG remains a failed pair; still attempt every
        # native document and both DPI values before writing the comparison.
        Write-Warning $_.Exception.Message
        if(!(Test-Path -LiteralPath (Join-Path $runFull "$mode\run.json"))){throw}
    }
}
$comparison=Join-Path $runFull 'comparison'
& $PythonPath (Join-Path $PSScriptRoot 'compare.py') --reference (Join-Path $runFull 'reference') --software (Join-Path $runFull 'software') --native (Join-Path $runFull 'native') --output $comparison
$compareExit=$LASTEXITCODE
Copy-Item -LiteralPath (Join-Path $comparison 'result.txt') -Destination (Join-Path $PSScriptRoot 'verification-result.txt')
if($regressionFailure){
    Add-Content -LiteralPath (Join-Path $PSScriptRoot 'verification-result.txt') -Value ("Regression failure: "+$regressionFailure) -Encoding UTF8
    throw 'Verification failed its regression checks. The complete document comparison and diagnostic artifacts are retained.'
}
if($compareExit -ne 0){throw 'Verification failed. Original PNGs/logs/build artifacts remain for diagnosis.'}
if($CleanOnSuccess){& (Join-Path $PSScriptRoot 'Clean-VerifiedArtifacts.ps1') -ComparisonDirectory $comparison -PythonPath $PythonPath}
Write-Output (Join-Path $PSScriptRoot 'verification-result.txt')
