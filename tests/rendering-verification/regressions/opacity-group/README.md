Forty groups cover twenty opacity values on white and dark backgrounds.
Each group has a positive-z-index child and a separately positioned sibling
to check that group compositing and stacking apply to all descendants.
Every coordinate/size is an even integer CSS pixel, so both 96 and 144 DPI
render axis-aligned geometry on whole device pixels.

The independent WebView2 oracle has 80 rectangles and 80 computed opacity
values per DPI. `OpacityGroupRegression.cpp` compares them for both DPI
values, layout and relayout, and all HTML IDs renamed (1,944 checks).
The computed CSS opacity stays a floating-point value; painting into an
eight-bit surface must not alter that observable property.

The first native rendering after the stacking-group correction still differed
from GPU WebView2 at 14,048 pixels (96 DPI) and 31,608 pixels (144 DPI).
Separate GPU/software captures showed different output from each other, and
native did not simply match software. These values are not blanket backend
exclusions. Full PNG comparison remains exact with no resizing or tolerance.

Rounding group alpha reduced the remaining differences to 3,712/8,352
pixels at 96/144 DPI. Thirty-eight of forty groups now match GPU output;
the two remaining groups use opacity=.1 and differ in the blue component
of the red background. These residual pixels are retained as failures.

`generate-oracle.py <capture-directory>` explicitly records the unmodified
reference rectangles/styles and hashes; normal regressions never regenerate
expectations. The matrix includes zero and one opacity, intermediate decimal
values and fractional values around byte boundaries. It uses no text, curves
or document-dependent calibration constants.
