"""Author an independent numeric-grid placement regression; no corpus IDs."""
from pathlib import Path

HERE = Path(__file__).resolve().parent
patterns = {
    "holes": [{"minor": "span 2"}, {"minor": "span 2"}, {},
              {"minor": "span 2"}, {}],
    "mixed": [{}, {"minor": "3"}, {}, {"minor": "1"}, {}],
    "locked": [{"major": "1 / span 2", "minor": "span 2"},
               {"major": "2"}, {"major": "1"}, {},
               {"major": "1", "minor": "2"}],
    "ordered": [{"minor": "span 2", "order": 2}, {"order": -2},
                {"minor": "3", "order": -1}, {"order": 1},
                {"major": "2", "order": 0}],
}
html = ['<!doctype html><html><head><meta charset="utf-8">',
        '<link rel="stylesheet" href="style.css"></head><body>']
colors = ["#285c83", "#b85627", "#44862d", "#7f4486", "#169399"]
group = 0
for flow in ("row", "column"):
    for dense in (False, True):
        for name, items in patterns.items():
            x, y = (group % 4) * 200 + 8, (group // 4) * 135 + 8
            group_id = f"{flow}-{'dense' if dense else 'sparse'}-{name}"
            direction = "rtl" if name == "ordered" else "ltr"
            html.append(f'<div id="{group_id}" data-probe class="grid" '
                        f'style="left:{x}px;top:{y}px;grid-auto-flow:{flow}'
                        f'{" dense" if dense else ""};direction:{direction}">')
            for index, item in enumerate(items):
                declarations = [f"background:{colors[index]}"]
                for axis in ("major", "minor"):
                    if axis in item:
                        property_name = ("grid-row" if (axis == "major") == (flow == "row")
                                         else "grid-column")
                        declarations.append(f"{property_name}:{item[axis]}")
                if "order" in item:
                    declarations.append(f'order:{item["order"]}')
                html.append(f'<div id="{group_id}-{index}" data-probe class="item" '
                            f'style="{";".join(declarations)}"></div>')
            html.append('</div>')
            group += 1
html.append('</body></html>')
HERE.joinpath("index.html").write_text("\n".join(html) + "\n", encoding="utf-8")
HERE.joinpath("style.css").write_text(
    "html,body{margin:0;background:white}body{width:800px;height:600px}\n"
    ".grid{position:absolute;display:grid;width:184px;height:116px;"
    "grid-template-columns:repeat(3,20px);grid-template-rows:repeat(3,16px);"
    "grid-auto-columns:20px;grid-auto-rows:16px;gap:2px;"
    "align-content:start;justify-content:start;align-items:start}"
    ".item{height:10px;min-width:0}\n", encoding="utf-8")
print("Created 16 grids / 96 probes without text or antialiased shapes")
