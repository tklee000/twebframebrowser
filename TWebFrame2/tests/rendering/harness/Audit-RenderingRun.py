"""Verify preserved pilot inputs/oracle pixels and the current captured source."""
import collections
import hashlib
import json
import sys
from pathlib import Path
from PIL import Image

root = Path(__file__).resolve().parents[4]
run = Path(sys.argv[1]).resolve()
corpus = root / 'TWebFrame2/tests/rendering'
previous = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else corpus / 'runs/20261003-184127-080-f8191e3e'
pilot = corpus / 'runs/20261003-151215-306-e689b9d7'
allow_reference_environment_change = '--reference-environment-change' in sys.argv[3:]
load = lambda p: json.loads(p.read_text(encoding='utf-8-sig'))
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest().upper()
summary, before = load(run / 'summary.json'), load(previous / 'summary.json')
environment = load(run / 'environment.json')
if 'backendExceptionRegistrySha256' in summary:
    assert sha(run / 'backend-pixel-exceptions.json') == summary['backendExceptionRegistrySha256']
original_inputs = {i['id']: i for i in load(pilot / 'input-manifest.json')}
for case in load(run / 'input-manifest.json'):
    for name in ('htmlSha256', 'cssSha256'):
        assert case[name] == original_inputs[case['id']][name], (case['id'], name)
    folder = corpus / 'fixtures' / case['id']
    assert sha(folder / 'index.html') == case['htmlSha256']
    assert sha(folder / 'style.css') == case['cssSha256']
for source in environment['sourceHashes']:
    assert sha(root / source['file']) == source['sha256'], source['file']
for font in environment['fontHashes']:
    assert sha(Path('C:/Windows/Fonts') / font['file']) == font['sha256'], font['file']
for runtime in environment.get('runtimeHashes', []):
    assert sha(run / runtime['file']) == runtime['sha256'], runtime['file']
old_rows = {(i['id'], i['dpi'], i['viewport']): i for i in before['results']}
pilot_rows = {(i['id'], i['dpi'], i['viewport']): i for i in load(pilot / 'summary.json')['results']}
fixed, regressed, fonts, runtimes = [], [], set(), set()
paint_progress = collections.defaultdict(lambda: dict(pairs=0, beforePixels=0, afterPixels=0))
failure_kinds, character_gates = collections.Counter(), collections.Counter()
stable, window_routes, glyph_characters, font_nodes = 0, 0, 0, 0
reference_routes = 0
reference_pixels_unchanged, native_pixels_unchanged = 0, 0
durations, peaks = [], []
for row in summary['results']:
    key = row['id'], row['dpi'], row['viewport']
    capture = Path(row['capturePath'])
    with Image.open(capture / 'reference.png') as current, Image.open(Path(pilot_rows[key]['capturePath']) / 'reference.png') as old:
        unchanged = current.size == old.size and current.convert('RGBA').tobytes() == old.convert('RGBA').tobytes()
        assert unchanged or allow_reference_environment_change, key
        reference_pixels_unchanged += int(unchanged)
    with Image.open(capture / 'native.png') as current, Image.open(Path(old_rows[key]['capturePath']) / 'native.png') as old:
        unchanged = current.size == old.size and current.convert('RGBA').tobytes() == old.convert('RGBA').tobytes()
        if allow_reference_environment_change:
            assert unchanged, key
            assert load(capture / 'reference.json') == load(Path(old_rows[key]['capturePath']) / 'reference.json'), key
        native_pixels_unchanged += int(unchanged)
    status, result = load(capture / 'capture-status.json'), load(capture / 'result.json')
    assert status['status'] == 'CAPTURED' and status['referenceStable'] and status['nativeStable'] and status['dpiTransitionStable'], key
    stable += 1
    if status.get('captureSchemaVersion', 0) >= 3:
        assert result['referenceRoute']['tested'] and result['referenceRoute']['matches'], key
        with Image.open(capture / 'reference.png') as preview, Image.open(capture / 'reference-cdp.png') as cdp:
            assert preview.size == cdp.size and preview.convert('RGBA').tobytes() == cdp.convert('RGBA').tobytes(), key
        reference_routes += 1
    runtimes.add(status['runtime'])
    durations.append(status['captureMilliseconds'])
    peaks.append(status['harnessPeakWorkingSetBytes'])
    if result['pass'] and result['differentPixels']:
        assert result['status'] == 'PASS_BACKEND_DIFFERENCE' and not result['strictPass'], key
        assert result['disallowedDifferentPixels'] == 0, key
        entry = result['backendException']
        assert entry['kind'] in ('CPU_GPU_FONT_COMPOSITION', 'CPU_GPU_GRADIENT_COMPOSITION', 'CPU_GPU_RASTER_ANTIALIASING'), key
        assert entry['differentPixels'] == result['differentPixels'], key
        for name in ('reference', 'native'):
            with Image.open(capture / (name + '.png')) as image:
                signature = hashlib.sha256(image.convert('RGBA').tobytes('raw', 'BGRA')).hexdigest().upper()
                assert signature == entry[name + 'PixelSha256'], key
    route = result['windowRoute']
    if row['dpi'] == route['windowDpi']:
        assert route['tested'] and route['matchesExplicit'], key
        window_routes += 1
    if row['pass'] and not old_rows[key]['pass']:
        fixed.append(list(key))
    if old_rows[key]['pass'] and not row['pass']:
        regressed.append(list(key))
    progress = paint_progress[f"{row['id']}@{row['dpi']}dpi"]
    progress['pairs'] += 1
    progress['beforePixels'] += old_rows[key]['differentPixels']
    progress['afterPixels'] += row['differentPixels']
    for failure in result.get('failures', []):
        failure_kinds[failure['kind']] += 1
    text = result.get('textGeometry', {})
    if text.get('referenceCharacters', 0) or text.get('nativeCharacters', 0):
        character_gates['passed' if text['passed'] else 'failed'] += 1
    native, reference = load(capture / 'native.json'), load(capture / 'reference.json')
    platform = load(capture / 'reference-fonts.json')
    assert len(platform['nodes']) == len(reference['boxes']), key
    for index, node in enumerate(platform['nodes']):
        assert index == node['trackedIndex'] and isinstance(node['usage']['fonts'], list), key
        font_nodes += 1
        for font in node['usage']['fonts']:
            assert font['familyName'] and font['postScriptName'] and font['glyphCount'] >= 0, key
    for char in native['textGeometry']:
        glyph = char['glyph']
        assert glyph['collected'] and glyph['fontResolved'] and glyph['index'] > 0 and glyph['postScript'], key
        glyph_characters += 1
        fonts.add(glyph['postScript'])
    width, height = map(int, row['viewport'].split('x'))
    with Image.open(capture / 'native.png') as image:
        assert image.size == (round(width * row['dpi'] / 96), round(height * row['dpi'] / 96)), key
assert len(summary['results']) == 120 and not regressed
if allow_reference_environment_change:
    assert environment['referenceGraphicsRecordsVerified'] == 12 and reference_routes == 120
    calibration = load(run / 'capture-calibration/summary.json')
    assert calibration['count'] == calibration['passed'] == 18 and calibration['failed'] == 0
assert len(runtimes) == 1 and runtimes == {load(Path(before['results'][0]['capturePath']) / 'capture-status.json')['runtime']}
controls = load(run / 'comparison-controls/controls.json')
assert len(controls) >= 28 and all(i['controlVerified'] and i['actualPass'] == i['expectedPass'] for i in controls)
old_archives = (pilot, corpus / 'runs/20261003-162837-939-d2a5124f', corpus / 'runs/20261003-174213-996-9c018c59')
for old_run in old_archives:
    catalog = corpus / 'archives' / (old_run.name + '.json')
    assert sha(catalog.with_suffix('.zip')) == load(catalog)['zipSha256']
audit = dict(schemaVersion=1, runId=run.name, originalInputHashesUnchanged=20,
             originalReferencePixelsUnchanged=reference_pixels_unchanged, stableCapturePairs=stable,
             referenceEnvironmentChangeRecorded=allow_reference_environment_change,
             previousNativePixelsUnchanged=native_pixels_unchanged,
             dpiTransitionStablePairs=stable, wmPrintClientMatches=window_routes,
             referenceCaptureRouteMatches=reference_routes,
             comparisonControlsVerified=len(controls), previousArchivesUnchanged=len(old_archives),
             comparedPreviousRun=previous.name, paintProgress=dict(paint_progress),
             currentSourceHashesMatched=len(environment['sourceHashes']), fontFilesHashed=len(environment['fontHashes']),
             passed=summary['passed'], failed=summary['failed'], newlyPassing=fixed, regressed=regressed,
             strictlyPassed=summary.get('strictlyPassed', summary['passed']),
             acceptedBackendDifferencePairs=summary.get('acceptedBackendDifferencePairs', 0),
             failureKinds=dict(failure_kinds), characterGates=dict(character_gates),
             nativeRepresentativeGlyphCharacters=glyph_characters, nativePostScriptFonts=sorted(fonts),
             referencePlatformFontNodes=font_nodes, runtime=next(iter(runtimes)),
             runElapsedSeconds=summary['elapsedSeconds'], maximumCaptureMilliseconds=max(durations),
             peakHarnessWorkingSetBytes=max(peaks), actualWindowsBothDpiValidated=False, fullContractComplete=False)
destination = run / 'continuation-audit.json'
assert not destination.exists(), 'Audit destination exists; preserve results'
destination.write_text(json.dumps(audit, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps(audit, ensure_ascii=False, indent=2))
