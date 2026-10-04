param([Parameter(Mandatory=$true)][string]$CatalogPath,[Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$CatalogPath=[IO.Path]::GetFullPath($CatalogPath);$Destination=[IO.Path]::GetFullPath($Destination)
$catalog=Get-Content -LiteralPath $CatalogPath -Raw -Encoding UTF8|ConvertFrom-Json
if([IO.Path]::GetFileName($catalog.zipFile) -cne $catalog.zipFile){throw 'Invalid archive filename'}
$zipPath=Join-Path (Split-Path $CatalogPath -Parent) $catalog.zipFile
if((Get-FileHash -LiteralPath $zipPath).Hash -cne $catalog.zipSha256){throw 'Archive SHA-256 mismatch'}
if(Test-Path -LiteralPath $Destination){throw 'Destination exists; restoration requires a new directory'}
$archive=[IO.Compression.ZipFile]::OpenRead($zipPath)
try{
    $reader=[IO.StreamReader]::new($archive.GetEntry('archive-manifest.json').Open())
    try{$manifest=@($reader.ReadToEnd()|ConvertFrom-Json)}finally{$reader.Dispose()}
    $seen=@{}
    foreach($file in $manifest){
        $target=[IO.Path]::GetFullPath((Join-Path $Destination $file.entry))
        if(-not $target.StartsWith($Destination.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or $seen.ContainsKey($target)){throw 'Invalid or duplicate archive path'}
        $seen[$target]=$true
        $entry=$archive.GetEntry($file.entry);if($null -eq $entry){throw ('Missing archive entry: '+$file.entry)}
        [IO.Directory]::CreateDirectory((Split-Path $target -Parent))|Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$target,$false)
        if((Get-FileHash -LiteralPath $target).Hash -cne $file.sha256 -or (Get-Item -LiteralPath $target).Length -ne $file.bytes){throw ('Restored hash mismatch: '+$file.entry)}
    }
}finally{$archive.Dispose()}
[IO.File]::Copy($CatalogPath,(Join-Path $Destination 'verified-archive-catalog.json'))
Write-Output ('Restored and verified '+$manifest.Count+' entries: '+$Destination)
