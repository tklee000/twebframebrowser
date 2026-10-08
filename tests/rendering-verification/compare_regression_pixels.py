"""Require exact, unmodified RGBA frames for independently captured regressions."""
import argparse
import json
from pathlib import Path
from PIL import Image
from compare import capture_metadata_valid, compare_pixels, read_json, sha


def run(root):
    cases = root.joinpath("cases.txt").read_text(encoding="utf-8-sig").splitlines()
    if not cases or any(not case.strip() for case in cases) or len({Path(case).name for case in cases}) != len(cases):
        raise ValueError("Regression fixtures are empty or duplicated")
    results = []
    for dpi in (96, 144):
        reference = root / f"reference-{dpi}"
        native = root / f"native-{dpi}"
        for folder, engine in ((reference, "webview2"), (native, "twebframe2")):
            summary = read_json(folder / "summary.json")
            if (summary.get("engine") != engine or summary.get("dpi") != dpi
                    or summary.get("attempted") != len(cases)
                    or summary.get("captured") != len(cases) or summary.get("errors") != 0):
                raise ValueError("Regression capture is incomplete or its DPI/engine differs")
        for case in cases:
            name = Path(case).name
            r, n = reference / name, native / name
            for folder in (r, n):
                if not capture_metadata_valid(read_json(folder / "status.json"), [800, 600], dpi):
                    raise ValueError("Regression status viewport/DPI/script mode differs")
                if (folder / "resource-error.txt").exists() and (folder / "resource-error.txt").read_text(encoding="utf-8-sig").strip():
                    raise ValueError("Regression resource loading failed")
            measured, snapshot = read_json(r / "layout.json"), read_json(n / "layout.json")
            if (measured.get("viewport") != [800, 600] or measured.get("dpr") != dpi / 96
                    or snapshot.get("width") != 800 or snapshot.get("height") != 600
                    or snapshot.get("dpi") != dpi):
                raise ValueError("Regression renderer metadata differs")
            with Image.open(r / "render.png") as reference_image, Image.open(n / "render.png") as native_image:
                size = (800 * dpi // 96, 600 * dpi // 96)
                if reference_image.size != size or native_image.size != size:
                    raise ValueError("Regression physical PNG dimensions differ")
                pixels, diff = compare_pixels(reference_image, native_image)
                if pixels["rawDifferentPixels"]:
                    diff.save(root / f"{name}-{dpi}-diff.png")
                results.append({"fixture": name, "dpi": dpi, **pixels,
                                "referencePngSha256": sha(r / "render.png"),
                                "nativePngSha256": sha(n / "render.png")})
    passed = sum(row["rawDifferentPixels"] == 0 for row in results)
    result = {"pairs": len(results), "passed": passed, "channelTolerance": 0,
              "backendExclusions": False, "resize": False, "results": results}
    root.joinpath("comparison.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    root.joinpath("result.txt").write_text(f"Exact PNG regression: {passed}/{len(results)} pairs passed\nRGBA tolerance 0; no backend exclusions or resizing\n", encoding="utf-8")
    print(f"Exact PNG regression: {passed}/{len(results)} pairs passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    raise SystemExit(run(parser.parse_args().directory))
