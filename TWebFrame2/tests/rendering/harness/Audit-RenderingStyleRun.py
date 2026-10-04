"""Verify the versioned style continuation without altering preserved captures."""
import argparse
import hashlib
import json
from pathlib import Path
from PIL import Image


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def pixels(path):
    with Image.open(path) as image:
        return image.size, hashlib.sha256(image.convert("RGBA").tobytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("run", type=Path)
    parser.add_argument("previous", type=Path)
    parser.add_argument("historical", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[4]
    output = args.run / "style-continuation-audit.json"
    if output.exists():
        raise ValueError("Audit exists; preserve it")
    env = read(args.run / "environment.json")
    old_env = read(args.previous / "environment.json")
    summary = read(args.run / "summary.json")
    old_summary = read(args.previous / "summary.json")
    inputs = read(args.run / "input-manifest.json")
    assert len(inputs) == 20 and inputs == read(args.previous / "input-manifest.json")
    assert summary["count"] == summary["passed"] == 120
    assert summary["referenceCaptureRouteMatches"] == 120
    assert env["fontHashes"] == old_env["fontHashes"]
    assert env["runtimeHashes"] == old_env["runtimeHashes"]
    assert env["windows"] == old_env["windows"]
    for source in env["sourceHashes"]:
        assert digest(repo / source["file"]) == source["sha256"], source["file"]
    for item in inputs:
        fixture = repo / "TWebFrame2/tests/rendering/fixtures" / item["id"]
        assert digest(fixture / "index.html") == item["htmlSha256"]
        assert digest(fixture / "style.css") == item["cssSha256"]
    previous_rows = {(r["id"], r["dpi"], r["viewport"]): r for r in old_summary["results"]}
    native_same = reference_same = historical_same = reference_diagnostics_same = 0
    checked = 0
    runtimes = set()
    for row in summary["results"]:
        relative = Path(f'{row["dpi"]}dpi') / row["viewport"] / row["id"]
        capture = args.run / relative
        previous = args.previous / relative
        result = read(capture / "result.json")
        status = read(capture / "capture-status.json")
        native = read(capture / "native.json")
        reference = read(capture / "reference.json")
        assert previous_rows[(row["id"], row["dpi"], row["viewport"])]["pass"]
        assert status["captureSchemaVersion"] == 4
        runtimes.add(status["runtime"])
        assert result["pass"] and not result["failures"]
        assert result["styleCoverage"]["contractVersion"] == 1
        assert result["styleCoverage"]["missing"] == result["styleCoverage"]["legacySkipped"] == 0
        assert result["styleCoverage"]["checked"] == len(reference["boxes"]) * 25
        assert all(len(b["computedStyles"]) == 25 for b in native["boxes"])
        assert result["referenceRoute"]["tested"] and result["referenceRoute"]["matches"]
        assert pixels(capture / "reference.png") == pixels(capture / "reference-cdp.png")
        native_same += pixels(capture / "native.png") == pixels(previous / "native.png")
        reference_same += pixels(capture / "reference.png") == pixels(previous / "reference.png")
        historical_same += pixels(capture / "reference.png") == pixels(args.historical / relative / "reference.png")
        reference.pop("styleContractVersion")
        for box in reference["boxes"]:
            box.pop("computedStyles")
        reference_diagnostics_same += reference == read(previous / "reference.json")
        checked += result["styleCoverage"]["checked"]
    assert len(runtimes) == 1
    assert native_same == reference_diagnostics_same == 120
    controls = read(args.run / "comparison-controls/controls.json")
    assert len(controls) == 134 and all(c["controlVerified"] for c in controls)
    calibration = read(args.run / "capture-calibration/summary.json")
    styles = read(args.run / "style-calibration/summary.json")
    assert calibration["count"] == calibration["passed"] == 18
    assert styles["count"] == styles["stylePassed"] == 18
    assert all(r["styleMissing"] == 0 for r in styles["results"])
    record = dict(schemaVersion=1, nativeDecodedPixelsUnchanged=native_same,
                  referenceDecodedPixelsUnchangedFromPrevious=reference_same,
                  referenceDecodedPixelsMatchingHistorical=historical_same,
                  previousReferenceDiagnosticsUnchanged=reference_diagnostics_same,
                  requiredStyleValuesChecked=checked, requiredStyleValuesMissing=0,
                  sourceHashesVerified=len(env["sourceHashes"]), preservedInputsVerified=20,
                  comparisonControls=len(controls), captureCalibrationPassed=18,
                  styleCalibrationStructurePassed=styles["stylePassed"],
                  styleCalibrationFullComparisonPassed=styles["passed"],
                  styleCalibrationFullComparisonFailed=styles["failed"],
                  styleExpectedValueChecks=styles["expectedValueChecks"],
                  strictlyPassed=summary["strictlyPassed"],
                  acceptedBackendDifferencePairs=summary["acceptedBackendDifferencePairs"],
                  runtime=next(iter(runtimes)), graphicsFingerprint=env["referenceGraphics"]["fingerprint"],
                  previousGraphicsFingerprint=old_env["referenceGraphics"]["fingerprint"],
                  fullContractComplete=False)
    output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
