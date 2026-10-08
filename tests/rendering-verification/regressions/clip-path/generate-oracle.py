"""Explicit authoring tool: retain independent WebView2 geometry and hit targets."""
import hashlib
import json
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
captures = Path(sys.argv[1]).resolve()
for name in ('clip-path', 'clip-path-rects'):
    fixture = root / 'regressions' / name
    runtimes = set()
    for dpi in (96, 144):
        folder = captures / f'reference-{dpi}'
        summary = json.loads((folder / 'summary.json').read_text(encoding='utf-8-sig'))
        assert summary['engine'] == 'webview2' and summary['captured'] == 2 and summary['errors'] == 0
        runtimes.add(summary['runtime'])
        measured = json.loads((folder / name / 'layout.json').read_text(encoding='utf-8-sig'))
        assert len(measured['boxes']) == 48 and len(measured['hits']) == 1008
        (fixture / f'expected-{dpi}.tsv').write_text(''.join(
            '\t'.join([box['id']] + [format(value, '.9g') for value in box['rect']]) + '\n'
            for box in measured['boxes']), encoding='utf-8')
        (fixture / f'hits-{dpi}.tsv').write_text(''.join(
            '\t'.join([format(x, '.9g'), format(y, '.9g'), hit]) + '\n'
            for x, y, hit in measured['hits']), encoding='utf-8')
    assert len(runtimes) == 1
    files = ['index.html', 'style.css', 'oracle-measure.js'] + [f'{prefix}-{dpi}.tsv' for prefix in ('expected', 'hits') for dpi in (96, 144)]
    (fixture / 'oracle.json').write_text(json.dumps({
        'oracle': 'Independent unmodified WebView2 rectangles and elementFromPoint',
        'runtime': runtimes.pop(), 'cssViewport': [800, 600], 'dpi': [96, 144],
        'shapes': 24, 'geometryProbes': 48, 'hitProbes': 1008,
        'engineReadsExpectedData': False,
        'inputAndExpectedSha256': {file: hashlib.sha256((fixture / file).read_bytes()).hexdigest() for file in files},
    }, indent=2) + '\n', encoding='utf-8')
print('Saved independent clip-path oracles: 96 rectangles + 2016 hit targets per DPI')
