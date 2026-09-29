# TWebFrame Browser

`TWebFrameBrowser.sln` is a separate native Windows browser application built on
the project's own `TWebFrame::View` renderer. It does not use WebView2, CEF, or
another embedded browser engine.

Open the solution in Visual Studio 2019 or later and build `Release|x64`.
The executable is `bin/Browser/x64/Release/TWebFrameBrowser.exe`. It opens the
Ppomppu free board at `https://www.ppomppu.co.kr/zboard/zboard.php?category=2&id=freeboard`.
On Ppomppu pages, empty script-driven ad slots and the ad-only sidebar are
collapsed so the board list starts directly below the board header.

The browser has tabs, an address and search field, Back, Forward, Reload/Stop,
Home, and page status. Ctrl+L focuses the address, Ctrl+T opens a tab, Ctrl+W
closes a tab, F5/Ctrl+R reloads, and Alt+Left/Right moves through history.

HTTPS and HTTP requests use Windows WinHTTP, including Windows TLS certificate
validation, redirects, session cookies, and gzip/deflate responses. Korean
EUC-KR pages are decoded with Windows code page 949.

The browser uses a bounded, priority-aware network pool (up to 16 workers and
six connections per host). It downloads CSS, scripts, iframe documents, and
images concurrently, coalesces duplicate in-flight URLs, and keeps a bounded
128 MiB/512-entry LRU response cache. Navigation cancellation drops queued work
and generation checks prevent results from an older page or closed tab from
being published.

TWebFrame uses a separate bounded CPU pool for HTML/CSS parsing, text-resource
loading, `fetch()`, `XMLHttpRequest`, and WIC image decoding. DOM adoption, JavaScript execution,
layout publication, events, HWND access, and Direct2D bitmap creation remain on
the UI thread. This keeps mutable page state single-threaded while moving the
blocking and CPU-heavy preparation stages off it.

## Current compatibility limit

`TWebFrame` is still a partial HTML/CSS/JavaScript engine and is not a sandbox.
The live browser downloads and executes page scripts by default so script-built
navigation and menus remain available. To inspect an incompatible or untrusted
page without running its scripts, start in static-page mode explicitly:

```text
TWebFrameBrowser.exe --disable-scripts https://example.com/
```

`--enable-scripts` is retained as a compatible explicit spelling of the default.
When scripts are enabled, external classic scripts are prefetched concurrently.
Unsupported syntax or Web APIs can be reported as handled first-chance C++
exceptions by Visual Studio; they do not by themselves mean that the process
crashed.
Layout, dynamic site behavior, forms, login, ads, media, and complex CSS may
differ from a standards browser. This solution displays live web HTML through
TWebFrame but does **not** establish complete rendering of Ppomppu or arbitrary
websites. Do not use it as a security boundary for hostile content.
