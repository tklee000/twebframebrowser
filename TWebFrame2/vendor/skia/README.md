# Skia CPU raster runtime

`libSkiaSharp.dll` is the unmodified Windows x64 native library from
[SkiaSharp.NativeAssets.Win32 4.153.1](https://www.nuget.org/packages/SkiaSharp.NativeAssets.Win32/4.153.1).
Package URL: `https://api.nuget.org/v3-flatcontainer/skiasharp.nativeassets.win32/4.153.1/skiasharp.nativeassets.win32.4.153.1.nupkg`.

- Package SHA-256: `C27B04338D1E4D400C20DE534356310B9971E123E081C732AF97E56F5572E1AE`
- DLL SHA-256: `935EF4A00462E6B0C4DADB870F734FA43679B4F561287FE0D28CBE2BA147E832`
- Original package path: `runtimes/win-x64/native/libSkiaSharp.dll`
- C ABI declarations: [SkiaSharp 4.153.1 bindings](https://github.com/mono/SkiaSharp/blob/v4.153.1/binding/SkiaSharp/SkiaApi.generated.cs).

The engine generates opaque two-stop linear gradient pixels on the CPU in
`RasterGradient.h`. Rounded gradient image brushes use a Skia raster surface
backed by CPU memory, with no GPU upload or readback. Other gradient brushes
use the existing shared Direct2D WIC software surface.

Solid nonuniform rounded corners, border rings and Gaussian shadows also use
Skia raster surfaces backed by CPU memory. The engine does not create a GL
context or perform GPU upload/readback. ANGLE is not deployed or loaded.

Inputs are the engine's own parsed CSS geometry, radii, brushes and View pixels.
DOM, CSS, layout, text shaping and glyph masks remain in TWebFrame. Text uses
DirectWrite coverage and CPU composition on the shared WIC surface.
No Chromium/WebView2 rendering API or reference PNG is used here.

`Directory.Build.targets` copies the DLL and both original license/notice files
beside x64 application executables. Loading uses that absolute path and system
dependency directories. Missing runtime falls back to the existing Direct2D
software path. CPU composition does not require a GL context. Win32 retains
Direct2D fallback.

Shadow tiles use a 256 pixel minimum, 32 pixel height alignment and
overlapping edge texels. Strict rendering comparisons keep zero tolerance.
The user-approved CPU/GPU pixel differences are recorded separately in each
accepted run and do not change comparator decisions or reference captures.
Runtime DLLs and license files are copied beside the saved renderer, hashed
in `environment.json`, and included with build inputs in `source-snapshot.zip`.
