19 independent HTML direction/cascade cases; 76 WebView2 rectangle/style probes at 96/144 DPI.

DirectionAttributeRegression checks initial layout, relayout, and an in-memory copy with every ID and corresponding CSS selector renamed. All 3,672 checks pass. Both 800x600 / 1200x900 PNG pairs have exactly equal RGBA pixels with no exclusions.

The earlier 72-probe geometry-only regression had 576 failures among 1,168 checks before the HTML direction fix. The final matrix additionally checks computed direction, Unicode bidi mode, logical padding, and ordinary RTL inline-block ordering.

The cases include case-insensitive valid values, invalid whitespace/empty values, inherited direction, author stylesheet/inline overrides, revert/initial/inherit/unset, logical padding, grid, flex/reversed flex, bdo/bdi, and inline-blocks. This fixture does not cover automatic first-strong direction or mixed text/atomic bidi reordering.

Expected TSV files are read by the regression project only. Production uses the shared CSS/DOM/layout paths without document-specific parameters. generate-oracle.py is an explicit authoring tool for separately captured WebView2 measurements and is never run by the regression or production engine.

HTML UA rules: https://html.spec.whatwg.org/multipage/rendering.html#bidi-rendering
