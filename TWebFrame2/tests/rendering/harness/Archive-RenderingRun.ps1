param([Parameter(Mandatory=$true)][string]$RunPath,[string]$ArchiveRoot='')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$RunPath=[IO.Path]::GetFullPath($RunPath)
$corpusRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(-not $ArchiveRoot){$ArchiveRoot=Join-Path $corpusRoot 'archives'}
$ArchiveRoot=[IO.Path]::GetFullPath($ArchiveRoot)
$summary=Get-Content -LiteralPath (Join-Path $RunPath 'summary.json') -Raw -Encoding UTF8|ConvertFrom-Json
$inputs=@(Get-Content -LiteralPath (Join-Path $RunPath 'input-manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json)
$archiveId=Split-Path $RunPath -Leaf
if($ArchiveRoot.StartsWith($RunPath.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or $ArchiveRoot -eq $RunPath){throw 'Archive destination must be outside the captured run'}
$zipPath=Join-Path $ArchiveRoot ($archiveId+'.zip')
$catalogPath=Join-Path $ArchiveRoot ($archiveId+'.json')
if((Test-Path -LiteralPath $zipPath) -or (Test-Path -LiteralPath $catalogPath)){throw 'Archive ID already exists; archives are immutable'}
[IO.Directory]::CreateDirectory($ArchiveRoot)|Out-Null
$files=[Collections.Generic.List[object]]::new()
Get-ChildItem -LiteralPath $RunPath -File -Recurse | Where-Object {$_.FullName.Substring($RunPath.Length+1) -notmatch '(^|[\\/])profile([\\/]|$)'} | ForEach-Object {
    $files.Add(@{path=$_.FullName;entry='run/'+$_.FullName.Substring($RunPath.Length+1).Replace('\','/')})
}
foreach($case in $inputs){
    if($case.id -notmatch '^\d{4}-[a-z0-9-]+$'){throw 'Invalid case ID in manifest'}
    $folder=Join-Path $corpusRoot ('fixtures\'+$case.id)
    if((Get-FileHash -LiteralPath (Join-Path $folder 'index.html')).Hash -ne $case.htmlSha256 -or (Get-FileHash -LiteralPath (Join-Path $folder 'style.css')).Hash -ne $case.cssSha256){throw ('Original fixture changed: '+$case.id)}
    Get-ChildItem -LiteralPath $folder -File -Recurse | ForEach-Object {$files.Add(@{path=$_.FullName;entry='inputs/'+$case.id+'/'+$_.FullName.Substring($folder.Length+1).Replace('\','/')})}
}
# Preserve reduced documents separately from the immutable authored corpus.
$reproRoot=Join-Path $corpusRoot 'repros'
if(Test-Path -LiteralPath $reproRoot){
    Get-ChildItem -LiteralPath $reproRoot -File -Recurse | ForEach-Object {
        $files.Add(@{path=$_.FullName;entry='repros/'+$_.FullName.Substring($reproRoot.Length+1).Replace('\','/')})
    }
}
$manifest=@($files|ForEach-Object {@{entry=$_.entry;sha256=(Get-FileHash -LiteralPath $_.path).Hash;bytes=(Get-Item -LiteralPath $_.path).Length}})
$archive=[IO.Compression.ZipFile]::Open($zipPath,[IO.Compression.ZipArchiveMode]::Create)
try{
    foreach($file in $files){[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$file.path,$file.entry,[IO.Compression.CompressionLevel]::Optimal)|Out-Null}
    $entry=$archive.CreateEntry('archive-manifest.json');$writer=[IO.StreamWriter]::new($entry.Open(),[Text.UTF8Encoding]::new($false))
    try{$writer.Write((ConvertTo-Json -InputObject $manifest -Depth 6))}finally{$writer.Dispose()}
}finally{$archive.Dispose()}
# Verify every decompressed file; compressed archive SHA alone cannot establish recovery.
$verified=[IO.Compression.ZipFile]::OpenRead($zipPath)
try{
    foreach($file in $manifest){
        $entry=$verified.GetEntry($file.entry);if($null -eq $entry -or $entry.Length -ne $file.bytes){throw ('Archive entry missing or truncated: '+$file.entry)}
        $stream=$entry.Open();$sha=[Security.Cryptography.SHA256]::Create()
        try{$actual=[BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','')}finally{$sha.Dispose();$stream.Dispose()}
        if($actual -cne $file.sha256){throw ('Archive recovery hash mismatch: '+$file.entry)}
    }
}finally{$verified.Dispose()}
$catalog=[ordered]@{schemaVersion=1;archiveId=$archiveId;createdUtc=[DateTime]::UtcNow.ToString('o');zipFile=(Split-Path $zipPath -Leaf);zipSha256=(Get-FileHash -LiteralPath $zipPath).Hash;bytes=(Get-Item -LiteralPath $zipPath).Length;entryCount=$manifest.Count;everyEntryRecoveryVerified=$true;sourceSnapshotIncluded=(Test-Path -LiteralPath (Join-Path $RunPath 'source-snapshot.zip'));rendererExecutableIncluded=(Test-Path -LiteralPath (Join-Path $RunPath 'RenderingComparisonRegression.exe'));excluded='Disposable WebView2 profile cache only; originals remain in run directory';summary=@{count=$summary.count;passed=$summary.passed;failed=$summary.failed;fullContractComplete=$false}}
[IO.File]::WriteAllText($catalogPath,($catalog|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
Write-Output ('Verified rendering archive: '+$zipPath)
