param([string]$BuildDirectory=(Join-Path $PSScriptRoot '.work\build'),[string]$PythonPath='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
$exe=Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'bin\RenderingCapture.exe'
if(!(Test-Path -LiteralPath $exe)){throw 'Build the capture host before running PNG regressions.'}
if(!$PythonPath){$PythonPath=Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'}
if(!(Test-Path -LiteralPath $PythonPath)){throw 'Pass -PythonPath for a runtime with Pillow and NumPy.'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $PSScriptRoot ('.work\exact-png-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))}
$outputFull=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $outputFull){throw 'Use a new output directory for PNG regressions.'}
[IO.Directory]::CreateDirectory($outputFull) | Out-Null
$fixtures=@('table-formatting','text-spacing','auto-table','text-wrapping','table-height','intrinsic-width','intrinsic-contributions','text-minimum','text-break-controls','soft-hyphen-paint','flex-sizing','direction-attribute','item-order','clip-path','clip-path-rects','opacity-group','grid-auto-flow','mixed-inline-flow','multicol-block-flow') | ForEach-Object {(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot ('regressions\'+$_))).Path}
$list=Join-Path $outputFull 'cases.txt'
[IO.File]::WriteAllLines($list,$fixtures,[Text.UTF8Encoding]::new($false))
$provenance=@{executableSha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant();fixtures=@{};dpi=@(96,144);cssViewport=@(800,600)}
foreach($fixture in $fixtures){
    foreach($file in @('index.html','style.css')){
        $inputFile=Join-Path $fixture $file
        $provenance.fixtures[$inputFile]=(Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$provenance | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputFull 'run.json') -Encoding UTF8
foreach($dpi in @(96,144)){
    foreach($engine in @('webview2','twebframe2')){
        $name=if($engine -eq 'webview2'){'reference'}else{'native'}
        $out=Join-Path $outputFull ($name+'-'+$dpi)
        & $exe --engine $engine --cases $list --out $out --width 800 --height 600 --dpi $dpi --timeout-ms 30000 --measure (Join-Path $PSScriptRoot 'measure.js') --software false
        if($LASTEXITCODE -ne 0){throw "PNG regression capture failed: $engine / $dpi DPI"}
    }
}
if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant() -ne $provenance.executableSha256){throw 'The capture executable changed during PNG regressions.'}
foreach($inputFile in $provenance.fixtures.Keys){
    if((Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne $provenance.fixtures[$inputFile]){throw 'A regression input changed during captures.'}
}
& $PythonPath (Join-Path $PSScriptRoot 'compare_regression_pixels.py') $outputFull
if($LASTEXITCODE -ne 0){throw 'Exact PNG regression failed; captures and differences are retained.'}
Write-Output ('PNG regression output: '+$outputFull)
