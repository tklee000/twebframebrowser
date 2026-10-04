"""Read-only pixel diagnostics. Regions do not grant any paint tolerance."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def diagnose(capture):
    reference = np.array(Image.open(capture / 'reference.png').convert('RGB'))
    native = np.array(Image.open(capture / 'native.png').convert('RGB'))
    if reference.shape != native.shape:
        raise ValueError(f'Image size mismatch: {capture}')
    delta = np.abs(reference.astype(np.int16) - native.astype(np.int16))
    different = np.any(delta != 0, axis=2)
    ys, xs = np.where(different)
    data = json.loads((capture / 'reference.json').read_text(encoding='utf-8-sig'))
    status = json.loads((capture / 'native.json').read_text(encoding='utf-8-sig'))
    scale = status['dpi'] / 96
    regions = []
    for box in data['boxes']:
        if not box.get('present') or box['id'] == 'body':
            continue
        x, y, width, height = box['rect']
        left, top = max(0, round(x * scale)), max(0, round(y * scale))
        right = min(reference.shape[1], round((x + width) * scale))
        bottom = min(reference.shape[0], round((y + height) * scale))
        if right <= left or bottom <= top:
            continue
        mask = different[top:bottom, left:right]
        values = delta[top:bottom, left:right]
        regions.append(dict(id=box['id'], deviceRect=[left, top, right, bottom],
                            differentPixels=int(mask.sum()),
                            maximumChannelDelta=int(values.max()),
                            meanChannelDelta=float(values.mean())))
    return dict(capturePath=str(capture.resolve()), dpi=status['dpi'],
                differentPixels=int(different.sum()),
                maximumChannelDelta=int(delta.max()),
                bounds=[int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1] if xs.size else None,
                channelDeltaHistogram={str(i): int(np.count_nonzero(delta.max(axis=2) == i)) for i in (1, 2, 3, 4, 8, 16, 32, 64, 128, 255)},
                regions=regions, diagnosticOnly=True, allowedPixels=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path, nargs='+')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    results = [diagnose(path) for path in args.capture]
    report = json.dumps(results, ensure_ascii=False, indent=2)
    if args.output:
        # Exclusive creation protects previous diagnostic evidence.
        with args.output.open('x', encoding='utf-8') as output:
            output.write(report)
    else:
        print(report)


if __name__ == '__main__':
    main()
