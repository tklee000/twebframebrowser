param([string]$BuildDirectory = (Join-Path $PSScriptRoot '.work\build'))
$ErrorActionPreference='Stop'
$buildFull=[IO.Path]::GetFullPath($BuildDirectory)
$msbuild='C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe'
if (!(Test-Path -LiteralPath $msbuild)) { throw 'Visual Studio 2019 MSBuild is required.' }
$bin=Join-Path $buildFull 'bin\'
$obj=Join-Path $buildFull 'obj\'
[IO.Directory]::CreateDirectory($buildFull) | Out-Null
& $msbuild (Join-Path $PSScriptRoot '..\..\TWebFrame2\TWebFrame.vcxproj') /t:Build /p:Configuration=Release /p:Platform=x64 "/p:OutDir=$bin" "/p:IntDir=$($obj)engine\" /m:2 /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw 'Common engine build failed.' }
& $msbuild (Join-Path $PSScriptRoot 'Capture.vcxproj') /t:Build /p:Configuration=Release /p:Platform=x64 /p:BuildProjectReferences=false "/p:OutDir=$bin" "/p:IntDir=$($obj)capture\" /m:2 /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw 'Capture host build failed.' }
$skia=Join-Path $PSScriptRoot '..\..\TWebFrame2\vendor\skia\libSkiaSharp.dll'
Copy-Item -LiteralPath $skia -Destination $bin
Write-Output (Join-Path $bin 'RenderingCapture.exe')
