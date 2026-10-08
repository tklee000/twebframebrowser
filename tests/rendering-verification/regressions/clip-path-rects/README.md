Companion matrix to [clip-path](../clip-path/README.md), with 24 shapes,
48 rectangles and 1,008 independent hit targets per DPI. It covers one to
four inset values, percentages, negative and overconstrained insets,
`calc()`, reference boxes in either syntax order, fill/stroke/view box aliases
for CSS layout boxes, nonzero/evenodd polygon filling, nested shapes,
positive/negative z-index descendants, initial/unset/inherit, and an
unresolved URL. Current inherit cases exercise inheritance from `none`.

`oracle-measure.js`, the expected TSV files, and `oracle.json` are retained
alongside the source. The authoring generator is in the sibling clip-path
directory. The normal regression run consumes the oracle without writing it.
Exact PNG comparison remains mandatory at both DPI values, including
fractional inset and calc edges, with no pixel tolerance or backend waiver.
