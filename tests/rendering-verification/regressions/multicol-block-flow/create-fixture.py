"""Block fragmentation and column geometry; sources remain independent of IDs."""
from pathlib import Path

HERE = Path(__file__).resolve().parent
cases = [
    ('two', 'column-count:2;column-gap:12px'),
    ('three', 'column-count:3;column-gap:12px'),
    ('width', 'column-width:100px;column-gap:12px'),
    ('combined', 'column-count:4;column-width:110px;column-gap:12px'),
    ('shorthand', 'column-count:4;columns:2 110px;column-gap:12px'),
    ('rtl', 'column-count:3;column-gap:12px;direction:rtl'),
    ('normal-gap', 'column-count:3;column-gap:normal;font-size:12px'),
    ('auto-fill', 'column-count:2;column-gap:12px;height:54px;column-fill:auto'),
    ('balanced-height', 'column-count:3;column-gap:12px;height:54px'),
    ('forced', 'column-count:2;column-gap:12px'),
    ('span', 'column-count:2;column-gap:12px'),
    ('margins', 'column-count:2;column-gap:12px'),
]
html = ['<!doctype html><html><head><meta charset="utf-8"><link rel="stylesheet" href="style.css"></head><body>']
heights = (12, 16, 20, 8, 16, 12)
for name, declarations in cases:
    html.append(f'<section class="panel" id="{name}" data-probe><div class="columns" id="{name}-columns" data-probe style="{declarations}">')
    for index, height in enumerate(heights):
        if name == 'span' and index == 3:
            html.append(f'<div class="spanner" id="{name}-span" data-probe></div>')
        extra = 'break-before:column;' if name == 'forced' and index == 2 else ''
        if name == 'margins':
            extra += 'margin-top:3px;margin-bottom:5px;'
        html.append(f'<div class="cell c{index}" id="{name}-cell-{index}" data-probe style="height:{height}px;{extra}"></div>')
    html.append(f'</div><div class="after" id="{name}-after" data-probe></div></section>')
html.append('</body></html>')
HERE.joinpath('index.html').write_text('\n'.join(html)+'\n', encoding='utf-8')
HERE.joinpath('style.css').write_text('''html,body{margin:0;background:white}
body{font:12px/18px Arial;display:grid;grid-template-columns:repeat(2,350px);gap:12px;align-items:start;padding:8px}
.panel{width:350px;padding:4px;border:1px solid #acbbc8;box-sizing:border-box}
.columns{position:relative}
.cell{break-inside:avoid;background:#83b5ce}
.c1,.c3,.c5{background:#d8b4b9}
.spanner{column-span:all;height:10px;background:#759c6c}
.after{height:4px;background:#566b7c;margin-top:3px}
''', encoding='utf-8')
