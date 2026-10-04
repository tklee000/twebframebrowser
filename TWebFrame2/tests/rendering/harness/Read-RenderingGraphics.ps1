function Read-RenderingGraphicsIdentity([string]$Path){
    $record=Get-Content -LiteralPath $Path -Raw -Encoding UTF8|ConvertFrom-Json
    if(-not $record.available -or -not $record.gpuPage.ready -or $record.gpuPage.url -cne 'edge://gpu/'){throw 'Reference graphics diagnostics are unavailable'}
    $info=$record.gpuPage.info
    $active=@($info.basicInfo|Where-Object {$_.description -match '^GPU\d+$' -and $_.value -match '\*ACTIVE\*$'})
    $renderer=@($info.basicInfo|Where-Object description -CEQ 'GL_RENDERER')
    $backend=@($info.basicInfo|Where-Object description -CEQ 'Skia Backend')
    if($active.Count -ne 1 -or $renderer.Count -ne 1 -or $backend.Count -ne 1){throw 'Reference graphics identity is incomplete'}
    $identity=[ordered]@{remoteSession=$record.remoteSession;windowDpi=$record.windowDpi;activeGpu=$active[0].value;renderer=$renderer[0].value;skiaBackend=$backend[0].value;featureStatus=$info.featureStatus;compositorInfo=$info.compositorInfo;displayInfo=$info.displayInfo}
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$fingerprint=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($identity|ConvertTo-Json -Depth 12 -Compress)))).Replace('-','')}finally{$sha.Dispose()}
    [pscustomobject]@{fingerprint=$fingerprint;identity=$identity;diagnosticPath=$Path}
}
