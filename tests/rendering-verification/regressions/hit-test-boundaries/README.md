This matrix checks a rounded box's own hit region separately from visible
overflow by its descendants. It covers plain border-radius, margin-box clips,
percentage and unequal corner radii, fractional positions, zero radii, and
circle/ellipse clips with calc() and a padding-box reference.

WebView2 captures eight original element rectangles and 336 elementFromPoint
targets at each of 96/144 DPI. ClipPathRegression also checks relayout and an
input variant with every HTML ID changed. Production code consumes only CSS
geometry; it does not read IDs or the retained expectations.

The authoring script takes a capture root containing reference-96 and
reference-144. Ordinary regression runs only read the TSV files. The existing
clip-path matrices and their expectations are retained unchanged.
