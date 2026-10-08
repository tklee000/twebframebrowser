This matrix checks common CSS clipping with independently captured WebView2
rectangles and `elementFromPoint` targets at an 800 × 600 CSS viewport and
96/144 DPI. It has 24 shapes, 48 element rectangles, and 1,008 hit locations
per DPI. All shapes and probes fit within the first viewport.

Cases cover inset shorthand and percentages, independent corner radii,
default/percentage/closest-side/farthest-side circle and ellipse radii,
one/two/four-component positions, an empty circle, concave polygons,
border/padding/content/margin boxes, nested clipping, clipping with overflow,
and grouping with opacity and scale. Rectangles are expected to remain the
DOM bounds of the original boxes, including transforms; clipping does not
resize those boxes. A positioned sibling checks whether positive z-index
descendants remain inside their parent's stacking group.

`ClipPathRegression.cpp` reads the retained oracle only in the test process.
It runs both matrices, both DPI values, initial layout and relayout, and an
input variant with every HTML ID changed. No input ID or oracle is consulted
by production CSS or layout code. Rectangle checks use a 0.02 CSS-pixel
threshold to compare serialization of independent floating-point geometry.
The separate full-frame PNG check permits zero RGBA channel difference,
without resizing or backend exclusions.

`oracle.json` records the WebView2 runtime and input/expected SHA-256 hashes.
`generate-oracle.py <capture-directory>` is an explicit reference-authoring
tool; regression runs do not regenerate or modify expectations. It expects
two cases in `reference-96` and `reference-144`. The initial 48-shape matrix
produced 6,676 failed checks out of 17,712 before common clipping support.

Clipping and reference-box rules follow
[CSS Masking](https://www.w3.org/TR/css-masking-1/#the-clip-path) and
[CSS Shapes](https://www.w3.org/TR/css-shapes-1/#basic-shape-functions).
Browser box hit tests also cover a one-device-pixel area; clip-path tests use
the continuous point, while geometry-box-only clips intersect the hit area.
This is verified at both DPI values against the retained browser data.

This project does not establish support for SVG clip-source URLs, `path()`,
the newer `shape()`/`xywh()`/`rect()` functions, rounded polygons, arbitrary
3D transforms, or all CSS transform orders/origins. A missing URL is tested
as an unresolved reference. PNG differences remain failures even when the
geometry and hit targets pass; those checks do not substitute for pixels.
