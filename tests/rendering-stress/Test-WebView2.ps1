param(
    [string]$CorpusPath = $PSScriptRoot,
    [string]$OutputDirectory = "",
    [string[]]$CaseId = @(),
    [switch]$All
)

$ErrorActionPreference = 'Stop'
$corpusFull = (Resolve-Path -LiteralPath $CorpusPath).Path
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $PSScriptRoot ("artifacts\webview2-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$outputFull = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputFull) { throw "Use a new output directory: $outputFull" }
[IO.Directory]::CreateDirectory($outputFull) | Out-Null
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$sdkRoot = Join-Path $repoRoot 'packages\Microsoft.Web.WebView2.1.0.3595.46'
$coreDll = Join-Path $sdkRoot 'lib\net462\Microsoft.Web.WebView2.Core.dll'
$formsDll = Join-Path $sdkRoot 'lib\net462\Microsoft.Web.WebView2.WinForms.dll'
$loaderDll = Join-Path $sdkRoot 'build\native\x64\WebView2Loader.dll'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
foreach ($dependency in @($coreDll, $formsDll, $loaderDll, $compiler)) {
    if (!(Test-Path -LiteralPath $dependency)) { throw "Missing dependency: $dependency" }
}
$source = Join-Path $PSScriptRoot 'WebView2Smoke.cs'
$exe = Join-Path $outputFull 'WebView2Smoke.exe'
& $compiler /nologo /target:winexe /platform:x64 "/out:$exe" "/reference:$coreDll" "/reference:$formsDll" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll /reference:System.Web.Extensions.dll $source
if ($LASTEXITCODE -ne 0) { throw 'WebView2 smoke host compilation failed' }
foreach ($dependency in @($coreDll, $formsDll, $loaderDll)) {
    Copy-Item -LiteralPath $dependency -Destination $outputFull
}
$inputManifest = Join-Path $outputFull 'inputs.json'
$runMode = if ($CaseId.Count) { 'selected' } elseif ($All) { 'all' } else { 'sample' }
@{ corpus=$corpusFull; output=$outputFull; mode=$runMode; caseIds=@($CaseId) } | ConvertTo-Json | Set-Content -LiteralPath $inputManifest -Encoding UTF8
# The helper owns an off-screen WinForms host. No visible interactive window is needed.
$process = Start-Process -FilePath $exe -ArgumentList ('"' + $inputManifest + '"') -WindowStyle Hidden -PassThru
while (!$process.WaitForExit(1000)) {
    if (Test-Path -LiteralPath (Join-Path $outputFull 'progress.txt')) {
        $latest = Get-Content -LiteralPath (Join-Path $outputFull 'progress.txt') -Raw
        if ($latest -ne $previous) { Write-Output $latest.Trim(); $previous = $latest }
    }
}
$summaryPath = Join-Path $outputFull 'summary.json'
if (!(Test-Path -LiteralPath $summaryPath)) {
    $fatalPath = Join-Path $outputFull 'fatal.txt'
    if (Test-Path -LiteralPath $fatalPath) { Get-Content -LiteralPath $fatalPath }
    throw "No WebView2 summary; exit $($process.ExitCode)"
}
$summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$summary | Add-Member -NotePropertyName hostSourceSha256 -NotePropertyValue $sourceHash
$summary | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
$summary | Select-Object runtimeVersion,mode,navigatedCases,viewportMeasurements,screenshots,failures,output | Format-List
if ($process.ExitCode -ne 0 -or $summary.failures -ne 0) { throw "WebView2 smoke failures; inspect $summaryPath" }
