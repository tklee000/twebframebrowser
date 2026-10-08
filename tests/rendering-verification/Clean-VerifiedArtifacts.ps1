param([Parameter(Mandatory=$true)][string]$ComparisonDirectory,[string]$PythonPath='')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$work=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '.work')).TrimEnd('\')
$comparison=(Resolve-Path -LiteralPath $ComparisonDirectory).Path.TrimEnd('\')
if(!$comparison.StartsWith($work+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Comparison must be inside the verification .work directory.'}
$result=Get-Content -LiteralPath (Join-Path $comparison 'comparison.json') -Raw | ConvertFrom-Json
$manifestPath=Join-Path $workspace 'tests\rendering-stress\manifest.json'
$manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if($manifest.cases.Count -ne 1000 -or $result.documents -ne 1000 -or $result.matrixPairs -ne 2000 -or $result.passedPairs -ne 2000 -or
   ($result.dpi -join ',') -ne '96,144' -or $result.channelTolerance -ne 0 -or $result.resize -or $result.specificCaseCorrections){
    throw 'Cleanup requires all 1,000 documents at both DPI values to pass the strict comparison.'
}
if($result.manifestSha256 -ne (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()){throw 'Original manifest changed.'}
foreach($dpi in @(96,144)){
    $rows=@($result.results | Where-Object {$_.dpi -eq $dpi})
    if($rows.Count -ne 1000 -or @($rows.id | Sort-Object -Unique).Count -ne 1000){throw 'Comparison matrix is incomplete or duplicated.'}
    foreach($case in $manifest.cases){
        $row=@($rows | Where-Object {$_.id -eq $case.id})
        if($row.Count -ne 1 -or $row[0].status -notlike 'PASS_*' -or $row[0].unexplainedPixels -ne 0){throw 'A case remains unverified.'}
    }
}
foreach($case in $manifest.cases){
    $folder=Join-Path (Split-Path $manifestPath) $case.path
    foreach($pair in @(@('index.html','htmlSha256'),@('style.css','cssSha256'))){
        if((Get-FileHash -LiteralPath (Join-Path $folder $pair[0]) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $case.($pair[1])){throw 'A preserved input changed.'}
    }
}
$catalogPath=Join-Path $PSScriptRoot 'preserved\catalog.json'
$catalog=Get-Content -LiteralPath $catalogPath -Raw | ConvertFrom-Json
$archive=Join-Path $PSScriptRoot 'preserved\inputs-tools-and-engine-before.zip'
if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $catalog.archiveSha256){throw 'Preservation archive hash mismatch.'}
$active=Get-CimInstance Win32_Process -Filter "Name='RenderingCapture.exe'" | Where-Object {$_.ExecutablePath -and $_.ExecutablePath.StartsWith($work+'\',[StringComparison]::OrdinalIgnoreCase)}
if($active){throw 'A capture process is still running.'}
Copy-Item -LiteralPath (Join-Path $comparison 'result.txt') -Destination (Join-Path $PSScriptRoot 'verification-result.txt')
if(!$PythonPath){$PythonPath=Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'}
if(!(Test-Path -LiteralPath $PythonPath)){throw 'Pass -PythonPath to preserve verified tools before cleanup.'}
& $PythonPath (Join-Path $PSScriptRoot 'preserve_tools.py')
if($LASTEXITCODE -ne 0){throw 'Tools/source preservation failed. No artifacts were removed.'}
$targets=@($work,[IO.Path]::GetFullPath((Join-Path $workspace 'tests\rendering-stress\artifacts')))
foreach($target in $targets){
    $resolved=[IO.Path]::GetFullPath($target).TrimEnd('\')
    if(!$resolved.StartsWith($workspace+'\tests\',[StringComparison]::OrdinalIgnoreCase) -or $resolved -notin $targets){throw 'Unexpected cleanup target.'}
    if(Test-Path -LiteralPath $resolved){
        $item=Get-Item -LiteralPath $resolved -Force
        if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Cleanup target must not be a junction/symlink.'}
        if(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}){throw 'A cleanup subtree contains a junction/symlink.'}
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
Write-Output 'Verified PNGs, detailed logs, profiles, executables and objects removed. Inputs/tools/regression projects/preservation archives retained.'
