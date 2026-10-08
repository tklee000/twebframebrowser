19 independent flex sizing cases; 64 WebView2 probes at 96/144 DPI.

FlexSizingRegression checks initial layout, relayout, and an in-memory copy with every ID renamed. The 520 original checks had 136 failures before the fix; the final 1,040 checks pass. Both 800x600 / 1200x900 PNG pairs have exactly equal RGBA pixels with no exclusions.

Expected TSV files are read by the regression project only. Production uses the shared CSS/DOM/layout paths without document-specific parameters.

The cases cover minimum/maximum freezing, partial grow/shrink factors, auto basis, box sizing, margins/padding/gaps, percentage limits, reversed/RTL rows, wrapping, and column main sizes.
