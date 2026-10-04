param([Parameter(Mandatory=$true)][string]$RunPath,[Parameter(Mandatory=$true)][string]$PreviousRunPath)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Import-PixelComparison.ps1')
$RunPath=[IO.Path]::GetFullPath($RunPath)
$output=Join-Path $RunPath 'transition-controls'
if(Test-Path -LiteralPath $output){throw 'Transition controls exist; use a new run'}
$tests=@(
    @{name='changed-engine';message='unchanged engine sources'},
    @{name='incomplete-calibration';message='complete independent color/DPI calibration'},
    @{name='missing-capture-route';message='complete capture-route equality'},
    @{name='software-reference';message='stable recorded GPU renderer'},
    @{name='changed-reference-geometry';message='Reference DOM, computed style or geometry changed'},
    @{name='changed-native-pixel';message='Native pixels changed'}
)
$results=@()
foreach($test in $tests){
    $folder=Join-Path $output $test.name
    [IO.Directory]::CreateDirectory((Join-Path $folder 'capture-calibration'))|Out-Null
    $environment=Get-Content -LiteralPath (Join-Path $RunPath 'environment.json') -Raw|ConvertFrom-Json
    $summary=Get-Content -LiteralPath (Join-Path $RunPath 'summary.json') -Raw|ConvertFrom-Json
    $calibration=Get-Content -LiteralPath (Join-Path $RunPath 'capture-calibration\summary.json') -Raw|ConvertFrom-Json
    switch($test.name){
        'changed-engine' {($environment.sourceHashes|Where-Object {$_.file -match 'src[\\/]RasterSurface.h$'}).sha256='0'*64}
        'incomplete-calibration' {$calibration.passed=17;$calibration.failed=1}
        'missing-capture-route' {$summary.referenceCaptureRouteMatches=119}
        'software-reference' {$environment.referenceGraphics.identity.featureStatus.featureStatus.gpu_compositing='disabled_software'}
        default {
            foreach($dpi in $environment.dpi){foreach($viewport in $environment.viewports){
                $matrix=$dpi.ToString()+'dpi\'+$viewport
                [IO.Directory]::CreateDirectory((Join-Path $folder $matrix))|Out-Null
                foreach($phase in @('before','after')){[IO.File]::Copy((Join-Path $RunPath ($matrix+'\reference-graphics-'+$phase+'.json')),(Join-Path $folder ($matrix+'\reference-graphics-'+$phase+'.json')))}
            }}
            $capture=Join-Path $folder 'altered-capture'
            [IO.Directory]::CreateDirectory($capture)|Out-Null
            foreach($file in @('native.png','reference.json')){[IO.File]::Copy((Join-Path $summary.results[0].capturePath $file),(Join-Path $capture $file))}
            $summary.results[0].capturePath=$capture
            if($test.name -eq 'changed-reference-geometry'){
                $reference=Get-Content -LiteralPath (Join-Path $capture 'reference.json') -Raw|ConvertFrom-Json
                $reference.boxes[0].rect[0]+=1
                [IO.File]::WriteAllText((Join-Path $capture 'reference.json'),($reference|ConvertTo-Json -Depth 20),[Text.UTF8Encoding]::new($false))
            }else{
                $image=Join-Path $capture 'native.png';$bitmap=[Drawing.Bitmap]::new($image)
                try{$copy=[Drawing.Bitmap]::new($bitmap)}finally{$bitmap.Dispose()}
                try{$copy.SetPixel(0,0,[Drawing.Color]::Magenta);$copy.Save($image,[Drawing.Imaging.ImageFormat]::Png)}finally{$copy.Dispose()}
            }
        }
    }
    foreach($pair in @(@{name='environment.json';value=$environment},@{name='summary.json';value=$summary},@{name='capture-calibration\summary.json';value=$calibration})){
        [IO.File]::WriteAllText((Join-Path $folder $pair.name),($pair.value|ConvertTo-Json -Depth 20),[Text.UTF8Encoding]::new($false))
    }
    $registry=Join-Path $folder 'rejected-registry.json';$message=''
    try{& (Join-Path $PSScriptRoot 'Register-RenderingBackendExceptions.ps1') -RunPath $folder -PreviousRunPath $PreviousRunPath -ReferenceEnvironmentChange -ApprovalReason 'Negative calibration; must be rejected' -RegistryPath $registry|Out-Null}
    catch{$message=$_.Exception.Message}
    if(-not $message.Contains($test.message) -or (Test-Path -LiteralPath $registry)){throw ('Reference transition guard failed: '+$test.name+' '+$message)}
    $results+=@{name=$test.name;rejected=$true;registryWritten=$false;message=$message}
}
[IO.File]::WriteAllText((Join-Path $output 'controls.json'),(ConvertTo-Json -InputObject $results -Depth 6),[Text.UTF8Encoding]::new($false))
Write-Output ('Verified '+$results.Count+' reference transition rejection controls: '+$output)
