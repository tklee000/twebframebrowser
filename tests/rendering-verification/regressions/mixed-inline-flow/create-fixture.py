"""Mixed inline line breaking, with no production ID-dependent behavior."""
from pathlib import Path

HERE = Path(__file__).resolve().parent
examples = [
    ("short", 180, "Alpha beta gamma delta epsilon zeta", ""),
    ("narrow", 120, "Alpha beta gamma delta epsilon zeta", ""),
    ("wide", 300, "Alpha beta gamma delta epsilon zeta eta theta", ""),
    ("fractional", 167.5, "Dashboard summary includes several short words", ""),
    ("collapse", 170, "  Alpha   beta\n gamma\t delta epsilon ", ""),
    ("nowrap", 140, "Alpha beta gamma delta epsilon zeta", "white-space:nowrap"),
    ("unbreakable", 155, "Alpha ABCDEFGHIJKLMNOPQRSTUVWXYZ beta gamma", "overflow-wrap:normal"),
    ("emergency", 155, "Alpha ABCDEFGHIJKLMNOPQRSTUVWXYZ beta gamma", "overflow-wrap:anywhere"),
    ("different-font", 190, "Alpha beta gamma delta epsilon zeta eta", "font-size:13.5px;line-height:1.4"),
    ("word-spacing", 170, "Alpha beta gamma delta epsilon zeta eta", "word-spacing:2px"),
    ("nested", 180, "Alpha beta <em>gamma delta epsilon zeta</em> eta theta", ""),
    ("trailing-inline", 180, "Alpha beta gamma delta epsilon <b>zeta</b> eta theta", ""),
]
html = ['<!doctype html><html><head><meta charset="utf-8"><link rel="stylesheet" href="style.css"></head><body>']
for name, width, text, extra in examples:
    html.append(f'<section class="sample" style="width:{width}px;{extra}" id="{name}" data-probe>'
                f'<div class="line" id="{name}-line" data-probe><span class="badge" id="{name}-badge" data-probe>00</span>'
                f' {text}<i class="tail" id="{name}-tail" data-probe>END</i></div>'
                f'<div class="after" id="{name}-after" data-probe></div></section>')
html.append('</body></html>')
HERE.joinpath('index.html').write_text('\n'.join(html)+'\n', encoding='utf-8')
HERE.joinpath('style.css').write_text('''html,body{margin:0;background:white}
body{font:12px/18px Arial;color:#172e40;display:grid;grid-template-columns:repeat(3,320px);gap:12px;align-items:start;padding:8px}
.sample{padding:4px;border:1px solid #acbbc8;box-sizing:border-box}
.badge{display:inline-block;background:#236747;color:white;font:10px/12px Consolas;padding:1px 4px;vertical-align:2px}
.tail{font-style:normal;color:#7a231c}
.after{height:4px;background:#accbdc;margin-top:3px}
''', encoding='utf-8')
