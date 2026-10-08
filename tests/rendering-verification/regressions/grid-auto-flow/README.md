Sixteen independently measured grids cover row/column flow, sparse/dense
packing, holes left by spans, mixed definite/automatic placement, locked
rows/columns and order with RTL. The original 1,000-document corpus is unchanged.

GridAutoFlowRegression uses 96 WebView2 probes at each of 96/144 DPI, initial
layout, relayout, renamed identifiers and unchanged DOM order. Before the common
placement fix, 280 of 1,560 checks failed. After the fix all 1,560 checks pass;
both PNG pairs have exactly equal RGBA pixels, with no backend exclusions.

The expected TSV files are regression inputs only. The independent oracle
authoring script does not run in the engine or the regression runner.

Placement follows https://www.w3.org/TR/css-grid-1/#auto-placement-algo .
