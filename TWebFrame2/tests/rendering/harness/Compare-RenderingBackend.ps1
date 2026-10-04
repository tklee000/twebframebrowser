function Find-RenderingBackendException {
    param([string]$ReferencePath, [string]$NativePath, [string]$CaseId,
          [int]$Dpi, [string]$Viewport, $Pixel,
          [string]$RegistryPath = (Join-Path $PSScriptRoot '../backend-pixel-exceptions.json'))
    if (-not $Pixel.DifferentPixels -or -not (Test-Path -LiteralPath $RegistryPath)) { return $null }
    $registry = Get-Content -Raw -LiteralPath $RegistryPath -Encoding UTF8 | ConvertFrom-Json
    if ($registry.schemaVersion -ne 1) { throw 'Unsupported backend pixel exception registry' }
    $candidates = @($registry.exceptions | Where-Object {
        $_.caseId -ceq $CaseId -and $_.dpi -eq $Dpi -and $_.viewport -ceq $Viewport
    })
    if (-not $candidates.Count) { return $null }
    $referenceHash = [StrictRenderingPixels]::PixelSha256($ReferencePath)
    $nativeHash = [StrictRenderingPixels]::PixelSha256($NativePath)
    foreach ($entry in $candidates) {
        if ($entry.kind -notin @('CPU_GPU_FONT_COMPOSITION', 'CPU_GPU_GRADIENT_COMPOSITION', 'CPU_GPU_RASTER_ANTIALIASING')) {
            throw 'Unsupported backend pixel exception kind'
        }
        if ($entry.referencePixelSha256 -ceq $referenceHash -and
            $entry.nativePixelSha256 -ceq $nativeHash -and
            $entry.differentPixels -eq $Pixel.DifferentPixels -and
            $entry.maximumChannelDelta -eq $Pixel.MaximumChannelDelta) {
            return $entry
        }
    }
    return $null
}
