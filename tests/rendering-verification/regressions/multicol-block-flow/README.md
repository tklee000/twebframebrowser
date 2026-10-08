Independent WebView2 regression for column count/width/shorthand, default gap,
RTL, auto and balanced filling, forced column breaks, spanning blocks and
adjacent block margins. The initial implementation targets indivisible blocks
(break-inside: avoid or atomic boxes). Line-level fragmentation inside ordinary
paragraphs, widows/orphans and column rules require separate coverage.
All 1,776 independent coordinate/DOM checks pass, including repeated layout
and renamed IDs. The entire PNG is exactly equal at both 96 and 144 DPI.
