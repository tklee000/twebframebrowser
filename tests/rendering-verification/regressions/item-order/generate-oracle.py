"""Save separately captured WebView2 reference data for CSS order regressions."""
import hashlib
import json
import sys
from pathlib import Path

fixture = Path(__file__).resolve().parent
captures = Path(sys.argv[1]).resolve()
runtimes = set()
for dpi in (96, 144):
    folder = captures / f"reference-final-{dpi}"
    summary = json.loads((folder / "summary.json").read_text(encoding="utf-8-sig"))
    assert summary["engine"] == "webview2" and summary["captured"] == 1 and summary["errors"] == 0
    runtimes.add(summary["runtime"])
    measured = json.loads((folder / "item-order" / "layout.json").read_text(encoding="utf-8-sig"))
    assert len(measured["boxes"]) == 120
    (fixture / f"expected-{dpi}.tsv").write_text("".join(
        "\t".join([box["id"]] + [format(value, ".9g") for value in box["rect"]]) + "\n"
        for box in measured["boxes"]), encoding="utf-8")
assert len(runtimes) == 1
files = ("index.html", "style.css", "oracle-measure.js", "expected-96.tsv", "expected-144.tsv")
(fixture / "oracle.json").write_text(json.dumps({
    "oracle": "Independent unmodified WebView2 DOM rectangles",
    "runtime": runtimes.pop(), "cssViewport": [800, 600], "dpi": [96, 144],
    "probes": 120, "groups": 24, "engineReadsExpectedData": False,
    "inputAndExpectedSha256": {name: hashlib.sha256((fixture / name).read_bytes()).hexdigest() for name in files},
}, indent=2) + "\n", encoding="utf-8")
print("Saved independent CSS order/alignment oracle: 120 probes at 96/144 DPI")
