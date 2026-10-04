param([Parameter(Mandatory=$true)][string]$RunPath,[Parameter(Mandatory=$true)][string]$CatalogPath)
$ErrorActionPreference='Stop'
$RunPath=[IO.Path]::GetFullPath($RunPath);$CatalogPath=[IO.Path]::GetFullPath($CatalogPath)
$corpusRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$environment=Get-Content -LiteralPath (Join-Path $RunPath 'environment.json') -Raw -Encoding UTF8|ConvertFrom-Json
$summary=Get-Content -LiteralPath (Join-Path $RunPath 'summary.json') -Raw -Encoding UTF8|ConvertFrom-Json
$catalog=Get-Content -LiteralPath $CatalogPath -Raw -Encoding UTF8|ConvertFrom-Json
$zipPath=Join-Path (Split-Path $CatalogPath -Parent) $catalog.zipFile
if((Get-FileHash -LiteralPath $zipPath).Hash -cne $catalog.zipSha256 -or -not $catalog.everyEntryRecoveryVerified){throw 'Baseline requires a verified rendering archive'}
$captures=@($summary.results|ForEach-Object {Get-Content -LiteralPath (Join-Path $_.capturePath 'capture-status.json') -Raw -Encoding UTF8|ConvertFrom-Json})
if(@($captures|Where-Object {$_.status -ne 'CAPTURED' -or -not $_.referenceStable}).Count){throw 'Incomplete or unstable reference cannot become a baseline'}
$versions=@($captures.runtime|Sort-Object -Unique);if($versions.Count -ne 1){throw 'Reference runtime changed'}
foreach($row in $summary.results){
    $capture=Get-Content -LiteralPath (Join-Path $row.capturePath 'capture-status.json') -Raw -Encoding UTF8|ConvertFrom-Json
    if($capture.captureSchemaVersion -ge 3){
        $result=Get-Content -LiteralPath (Join-Path $row.path 'result.json') -Raw -Encoding UTF8|ConvertFrom-Json
        if(-not $result.referenceRoute.tested -or -not $result.referenceRoute.matches){throw 'Uncalibrated reference capture cannot become a baseline'}
    }
}
$referenceEnvironment=[ordered]@{runtime=$versions[0];windows=$environment.windows;sdk=$environment.sdk;fontHashes=$environment.fontHashes;dpi=$environment.dpi;viewports=$environment.viewports;pageZoom=$environment.pageZoom;capture='WebView2 CapturePreview PNG; explicit rasterization scale; opaque BGRA8'}
$referenceEnvironment.measurementHashes=@($environment.sourceHashes|Where-Object {$_.file -match 'rendering[\\/]harness[\\/]measure\.js$|rendering[\\/]comparison-contract\.json$'})
if($environment.referenceCaptureCalibrationRequired){$referenceEnvironment.independentCapture='Exact CapturePreview / Page.captureScreenshot pixel equality; original CSS viewport at explicit rasterization scale'}
if($environment.referenceGraphics){$referenceEnvironment.graphics=$environment.referenceGraphics.identity;$referenceEnvironment.graphicsFingerprint=$environment.referenceGraphics.fingerprint}
if($environment.PSObject.Properties.Name -contains 'fontLocale'){$referenceEnvironment.fontLocale=$environment.fontLocale}
if($environment.PSObject.Properties.Name -contains 'enumeratedGraphicsAdapters'){$referenceEnvironment.enumeratedGraphicsAdapters=$environment.enumeratedGraphicsAdapters}
$referenceText=$referenceEnvironment|ConvertTo-Json -Depth 10 -Compress
$sha=[Security.Cryptography.SHA256]::Create()
try{$environmentId=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($referenceText))).Replace('-','').Substring(0,16).ToLowerInvariant()}finally{$sha.Dispose()}
$baselineId=$versions[0]+'-'+$environmentId+'-'+$catalog.archiveId
$destination=Join-Path $corpusRoot ('baselines\'+$baselineId)
if(Test-Path -LiteralPath $destination){throw 'Baseline ID exists; baseline indices are immutable'}
[IO.Directory]::CreateDirectory($destination)|Out-Null
$entries=@($summary.results|ForEach-Object {
    $row=$_;$relative=$row.capturePath.Substring($RunPath.Length+1).Replace('\','/')
    $referenceFiles=@('reference.png','reference.json','reference-metrics.json','reference-dom-snapshot.json')
    if(Test-Path -LiteralPath (Join-Path $row.capturePath 'reference-fonts.json')){$referenceFiles+='reference-fonts.json'}
    if(Test-Path -LiteralPath (Join-Path $row.capturePath 'reference-cdp.png')){$referenceFiles+=@('reference-cdp.png','reference-cdp-parameters.json')}
    $files=@($referenceFiles|ForEach-Object { @{entry='run/'+$relative+'/'+$_;sha256=(Get-FileHash -LiteralPath (Join-Path $row.capturePath $_)).Hash} })
    @{caseId=$row.id;dpi=$row.dpi;viewport=$row.viewport;files=$files}
})
$index=[ordered]@{schemaVersion=1;baselineId=$baselineId;environmentId=$environmentId;referenceEnvironment=$referenceEnvironment;archiveCatalog='../../archives/'+(Split-Path $CatalogPath -Leaf);archiveSha256=$catalog.zipSha256;storage='Immutable ZIP entries; restore with Restore-RenderingArchive.ps1';referencePairCount=$entries.Count;entries=$entries;note='Native failures are retained in the run archive; a baseline does not assert native conformance'}
if($environment.referenceGraphics){
    $index.graphicsFiles=@(foreach($dpi in $environment.dpi){foreach($viewport in $environment.viewports){foreach($phase in @('before','after')){
        $relative=$dpi.ToString()+'dpi/'+$viewport+'/reference-graphics-'+$phase+'.json'
        @{entry='run/'+$relative;sha256=(Get-FileHash -LiteralPath (Join-Path $RunPath $relative)).Hash}
    }}})
}
[IO.File]::WriteAllText((Join-Path $destination 'manifest.json'),($index|ConvertTo-Json -Depth 14),[Text.UTF8Encoding]::new($false))
Write-Output ('Immutable reference index: '+$destination)
