24 independent CSS order/alignment cases; 120 WebView2 probes at 96/144 DPI.

ItemOrderRegression checks initial layout, relayout, an in-memory copy with every ID renamed, and unchanged DOM traversal order. All 1,944 checks pass. Both 800x600 / 1200x900 PNG pairs have exactly equal RGBA pixels with no exclusions.

The initial 16-case / 80-probe matrix had 448 failures among 1,304 checks before sorting. Its source and independent expected data are kept in initial-80.zip. The final matrix adds grid inline alignment and physical auto margins; ordinary block items use 20px heights to avoid conflating CSS order with a separate inline baseline discrepancy.

Coverage includes negative/positive/equal order, LTR/RTL/reversed rows, wrapping and reversed wrapping, columns, flex/inline-flex, grid/inline-grid, explicit/automatic grid placement, overlapping paint order, ordinary block ordering, start/end/left/right/self-start/self-end alignment, and auto margins. The DOM is never reordered by production layout.

Expected TSV files are read by the regression project only. generate-oracle.py is an explicit authoring tool for independently captured WebView2 measurements and is never run by production or regression tests.

Standards: https://www.w3.org/TR/css-flexbox-1/#order-property
https://www.w3.org/TR/css-grid-1/#order-property
