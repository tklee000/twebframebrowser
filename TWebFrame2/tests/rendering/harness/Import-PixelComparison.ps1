$ErrorActionPreference='Stop'
if(-not ('StrictRenderingPixels' -as [type])){
    $drawingReferences=@([System.Drawing.Bitmap].Assembly.Location,[System.Drawing.Rectangle].Assembly.Location,
        [Security.Cryptography.SHA256].Assembly.Location,[Security.Cryptography.HashAlgorithm].Assembly.Location)
    if($PSVersionTable.PSEdition -eq 'Core'){
        $drawingReferences+=@('System.Runtime','System.Runtime.InteropServices','System.ComponentModel.Primitives')
        $drawingReferences+=@(Get-ChildItem -LiteralPath $PSHOME -Filter 'System.Private.Windows*.dll'|ForEach-Object FullName)
    }
    Add-Type -Path (Join-Path $PSScriptRoot 'StrictPixelComparison.cs') -ReferencedAssemblies ($drawingReferences|Select-Object -Unique)
}
