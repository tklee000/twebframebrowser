Mixed inline content after an atomic badge. Tests partial first-line space,
normal/nowrap/emergency wrapping, collapsed whitespace, fractional widths,
font/line-height/word spacing, nested inline elements and following inline text.
Coordinates are measured in an independent, unmodified WebView2 process.
The regression repeats layout and renames all IDs without changing DOM order.
It also checks original DOM Range/caret offsets and visible text pointer hits,
including collapsed spaces. A separate overflow document checks attached long
words and inline margins under anywhere/break-word/break-all, against independent
WebView2 viewport scroll sizes. The complete matrix has 1,908 checks at 96/144 DPI.
Detailed first-line cluster fragmentation for those overflow cases is not asserted.
Input and expected files are retained; generated captures/builds go in .work.
