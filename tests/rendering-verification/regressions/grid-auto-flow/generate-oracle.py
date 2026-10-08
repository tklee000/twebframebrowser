"""Save independent WebView2 measurements; production never reads this file."""
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
captures = Path(sys.argv[1]).resolve()
runtimes = set()
for dpi in (96, 144):
    folder = captures / f"reference-{dpi}"
    summary = json.loads(folder.joinpath("summary.json").read_text(encoding="utf-8-sig"))
    assert summary["engine"] == "webview2" and summary["captured"] == 1 and summary["errors"] == 0
    runtimes.add(summary["runtime"])
    measured = json.loads(folder.joinpath("grid-auto-flow", "layout.json").read_text(encoding="utf-8-sig"))
    assert measured["viewport"] == [800, 600] and measured["dpr"] == dpi / 96
    assert len(measured["boxes"]) == 96
    HERE.joinpath(f"expected-{dpi}.tsv").write_text("".join(
        "\t".join([box["id"]] + [format(value, ".9g") for value in box["rect"]]) + "\n"
        for box in measured["boxes"]), encoding="utf-8")
assert len(runtimes) == 1
files = ("index.html", "style.css", "expected-96.tsv", "expected-144.tsv")
HERE.joinpath("oracle.json").write_text(json.dumps({
    "oracle": "Independent unmodified WebView2 DOM rectangles",
    "runtime": runtimes.pop(), "cssViewport": [800, 600], "dpi": [96, 144],
    "probes": 96, "groups": 16, "engineReadsExpectedData": False,
    "inputAndExpectedSha256": {name: hashlib.sha256(HERE.joinpath(name).read_bytes()).hexdigest() for name in files},
}, indent=2) + "\n", encoding="utf-8")
