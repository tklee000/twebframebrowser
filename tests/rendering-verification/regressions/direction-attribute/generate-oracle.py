"""Save independent WebView2 measurements; never run this from the engine/tests."""
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
    measured = json.loads((folder / "direction-attribute" / "layout.json").read_text(encoding="utf-8-sig"))
    assert measured["viewport"] == [800, 600] and len(measured["boxes"]) == 76
    (fixture / f"expected-{dpi}.tsv").write_text("".join(
        "\t".join([box["id"]] + [format(value, ".9g") for value in box["rect"]]) + "\n"
        for box in measured["boxes"]), encoding="utf-8")
    (fixture / f"styles-{dpi}.tsv").write_text("".join(
        "\t".join(box[key] for key in ("id", "direction", "unicodeBidi", "paddingLeft", "paddingRight")) + "\n"
        for box in measured["boxes"]), encoding="utf-8")
assert len(runtimes) == 1
files = ["index.html", "style.css", "oracle-measure.js"] + [
    f"{kind}-{dpi}.tsv" for kind in ("expected", "styles") for dpi in (96, 144)]
provenance = {
    "oracle": "Independent unmodified WebView2 rectangles and computed styles",
    "runtime": runtimes.pop(), "cssViewport": [800, 600], "dpi": [96, 144],
    "probes": 76, "groups": 19, "engineReadsExpectedData": False,
    "inputAndExpectedSha256": {name: hashlib.sha256((fixture / name).read_bytes()).hexdigest() for name in files},
}
(fixture / "oracle.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
print("Saved independent direction oracle: 76 probes at 96/144 DPI")
