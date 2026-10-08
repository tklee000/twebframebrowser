"""Compare original device pixels without image resizing or per-case corrections."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture_metadata_valid(status, viewport, dpi):
    return (status.get("status") == "captured" and status.get("dpi") == dpi
            and status.get("cssViewport") == viewport
            and status.get("pixelSize") == [round(value * dpi / 96) for value in viewport]
            and status.get("pageScriptsEnabled") is False)


def backend_proof(gpu_summary, cpu_summary, gpu_graphics, cpu_graphics):
    def features(graphics):
        return graphics.get("gpuPage", {}).get("info", {}).get("featureStatus", {}).get("featureStatus", {})
    gpu, cpu = features(gpu_graphics), features(cpu_graphics)
    if not gpu_summary.get("runtime") or gpu_summary["runtime"] != cpu_summary.get("runtime"):
        return False, "GPU/software runtime versions differ or are unavailable"
    if gpu_summary.get("softwareRequested") or not cpu_summary.get("softwareRequested"):
        return False, "Independent hardware/software modes were not requested"
    if any(gpu.get(k) != "enabled" or cpu.get(k) != "disabled_software" for k in ("gpu_compositing", "rasterization")):
        return False, "Runtime GPU feature status does not prove distinct hardware/software backends"
    return True, "Runtime version and actual compositing/rasterization feature status confirmed"


def compare_pixels(reference, native, software=None, backend_eligible=False):
    ref = np.asarray(reference.convert("RGBA"))
    actual = np.asarray(native.convert("RGBA"))
    if ref.shape != actual.shape:
        return {"status": "FAIL_DIMENSIONS", "referenceShape": list(ref.shape), "nativeShape": list(actual.shape)}, None
    raw = np.any(ref != actual, axis=2)
    backend = np.zeros(raw.shape, dtype=bool)
    if software is not None and backend_eligible:
        cpu = np.asarray(software.convert("RGBA"))
        if cpu.shape != ref.shape:
            return {"status": "FAIL_SOFTWARE_DIMENSIONS"}, None
        # Exclude only a pixel whose complete native RGBA equals the independently
        # captured software reference, at a pixel where GPU and software differ.
        backend = raw & np.any(cpu != ref, axis=2) & np.all(actual == cpu, axis=2)
    unexplained = raw & ~backend
    total = raw.size
    difference = np.abs(ref.astype(np.int16) - actual.astype(np.int16))
    count = int(unexplained.sum())
    raw_count = int(raw.sum())
    report = {"status": "FAIL_PIXELS" if count else "PASS_EXACT" if not raw_count else "PASS_CONFIRMED_BACKEND_PIXELS",
              "totalPixels": total, "rawDifferentPixels": raw_count, "confirmedBackendPixels": int(backend.sum()),
              "unexplainedPixels": count, "exactPixelPercent": 100 * (total - raw_count) / total,
              "explainedPixelPercent": 100 * (total - count) / total, "maxChannelDifference": int(difference.max())}
    diff = np.zeros_like(ref)
    diff[:, :, 3] = 255
    diff[backend, :3] = (0, 180, 255)
    diff[unexplained, :3] = (255, 32, 32)
    return report, Image.fromarray(diff, "RGBA")


def run(args):
    manifest = read_json(args.corpus / "manifest.json")
    native_run, reference_run = read_json(args.native / "run.json"), read_json(args.reference / "run.json")
    if native_run.get("engine") != "twebframe2" or reference_run.get("engine") != "webview2" or reference_run.get("softwareRequested"):
        raise ValueError("Reference/native capture roles are incorrect")
    expected_manifest = sha(args.corpus / "manifest.json")
    if any(r["manifestSha256"] != expected_manifest for r in (native_run, reference_run)):
        raise ValueError("Capture manifest hash differs from original inputs")
    if native_run["dpi"] != reference_run["dpi"] or native_run["width"] != reference_run["width"] or native_run["height"] != reference_run["height"]:
        raise ValueError("Capture viewport/DPI matrix differs")
    if args.software:
        software_run = read_json(args.software / "run.json")
        if software_run.get("engine") != "webview2" or not software_run.get("softwareRequested"):
            raise ValueError("Software reference is not a software WebView2 run")
        for field in ("manifestSha256", "width", "height", "dpi", "executableSha256"):
            if software_run.get(field) != reference_run.get(field):
                raise ValueError("Software reference provenance differs: " + field)
    if args.output.exists():
        raise ValueError("Use a new comparison output directory")
    args.output.mkdir(parents=True)
    cases = manifest["cases"]
    if args.case_id:
        unknown = set(args.case_id) - {c["id"] for c in cases}
        if unknown:
            raise ValueError("Unknown case ID")
        cases = [c for c in cases if c["id"] in args.case_id]
    results = []
    selected_dpi = args.dpi or native_run["dpi"]
    if not selected_dpi:
        raise ValueError("DPI matrix is empty")
    if len(selected_dpi) != len(set(selected_dpi)):
        raise ValueError("Selected DPI values are duplicated")
    if not set(selected_dpi) <= set(native_run["dpi"]):
        raise ValueError("Selected DPI is outside the captured matrix")
    for dpi in selected_dpi:
        matrix = f"{native_run['width']}x{native_run['height']}-{dpi}dpi"
        if not (args.native / matrix / "summary.json").exists() or not (args.reference / matrix / "summary.json").exists():
            raise ValueError("Selected DPI capture has not finished")
        expected = [round(native_run["width"] * dpi / 96), round(native_run["height"] * dpi / 96)]
        mode_ok, mode_reason = False, "No independent software-reference capture"
        if args.software:
            try:
                mode_ok, mode_reason = backend_proof(read_json(args.reference / matrix / "summary.json"),
                    read_json(args.software / matrix / "summary.json"), read_json(args.reference / matrix / "graphics.json"),
                    read_json(args.software / matrix / "graphics.json"))
            except (OSError, ValueError, TypeError):
                mode_reason = "Runtime/backend evidence is unavailable"
        for case in cases:
            rid = case["id"]
            n, r = args.native / matrix / rid, args.reference / matrix / rid
            item = {"id": rid, "dpi": dpi, "cssViewport": [native_run["width"], native_run["height"]], "expectedPixelSize": expected}
            if not all((p / name).exists() for p in (n, r) for name in ("render.png", "status.json")):
                item.update(status="FAIL_CAPTURE", referenceCaptured=(r / "status.json").exists(), nativeCaptured=(n / "status.json").exists())
                for key, folder in (("referenceError", r), ("nativeError", n)):
                    if (folder / "error.txt").exists():
                        item[key] = (folder / "error.txt").read_text(encoding="utf-8-sig")
                results.append(item)
                continue
            if (n / "resource-error.txt").exists() and (error := (n / "resource-error.txt").read_text(encoding="utf-8-sig").strip()):
                item.update(status="FAIL_NATIVE_RESOURCE", nativeError=error)
                results.append(item)
                continue
            viewport = [native_run["width"], native_run["height"]]
            try:
                if not all(capture_metadata_valid(read_json(folder / "status.json"), viewport, dpi) for folder in (n, r)):
                    raise ValueError("Capture status DPI/viewport/script metadata differs")
                native_layout, gpu_layout = read_json(n / "layout.json"), read_json(r / "layout.json")
                if (native_layout.get("dpi") != dpi or native_layout.get("width") != viewport[0]
                        or native_layout.get("height") != viewport[1]
                        or gpu_layout.get("viewport") != viewport or gpu_layout.get("dpr") != dpi / 96):
                    raise ValueError("Measured renderer DPI or CSS viewport differs")
            except (OSError, ValueError, TypeError) as error:
                item.update(status="FAIL_CAPTURE_METADATA", metadataError=str(error))
                results.append(item)
                continue
            with Image.open(r / "render.png") as reference, Image.open(n / "render.png") as native:
                if list(reference.size) != expected or list(native.size) != expected:
                    item.update(status="FAIL_DPI_DIMENSIONS", referenceSize=list(reference.size), nativeSize=list(native.size))
                else:
                    cpu = None
                    eligible = False
                    reason = mode_reason
                    if args.software and mode_ok:
                        s = args.software / matrix / rid
                        reason = "Independent software capture is missing or failed"
                        if (s / "render.png").exists() and (s / "status.json").exists():
                            cpu_layout = read_json(s / "layout.json")
                            eligible = capture_metadata_valid(read_json(s / "status.json"), viewport, dpi) and gpu_layout == cpu_layout
                            reason = "Independent GPU/software pixels and identical measured reference geometry/styles" if eligible else "GPU/software reference measurements differ"
                            cpu = Image.open(s / "render.png")
                    pixels, diff = compare_pixels(reference, native, cpu, eligible)
                    if cpu is not None:
                        cpu.close()
                    item.update(pixels, backendEligibility=reason)
                    item.update(referencePngSha256=sha(r / "render.png"), nativePngSha256=sha(n / "render.png"))
                    if diff is not None and pixels["rawDifferentPixels"] and not args.no_diff_images:
                        dest = args.output / matrix
                        dest.mkdir(exist_ok=True)
                        diff.save(dest / (rid + ".png"))
            results.append(item)
            if len(results) % 100 == 0:
                print(f"Compared matrix entries: {len(results)}/{len(cases) * len(selected_dpi)}", flush=True)
    counts = dict(Counter(r["status"] for r in results))
    summary = {"schemaVersion": 1, "manifestSha256": expected_manifest, "documents": len(cases), "matrixPairs": len(results),
               "dpi": selected_dpi, "cssViewport": [native_run["width"], native_run["height"]], "counts": counts,
               "channelTolerance": 0, "resize": False, "specificCaseCorrections": False,
               "passedPairs": sum(v for k, v in counts.items() if k.startswith("PASS_")),
               "referenceExecutableSha256": reference_run["executableSha256"], "nativeExecutableSha256": native_run["executableSha256"], "results": results}
    pixel_rows = [r for r in results if "totalPixels" in r]
    total_pixels = sum(r["totalPixels"] for r in pixel_rows)
    raw_pixels = sum(r["rawDifferentPixels"] for r in pixel_rows)
    unexplained_pixels = sum(r["unexplainedPixels"] for r in pixel_rows)
    summary.update(comparedPairs=len(pixel_rows), comparedPixels=total_pixels, rawDifferentPixels=raw_pixels,
                   unexplainedPixels=unexplained_pixels, confirmedBackendPixels=sum(r["confirmedBackendPixels"] for r in pixel_rows),
                   exactPixelPercent=100 * (total_pixels - raw_pixels) / total_pixels if total_pixels else None,
                   explainedPixelPercent=100 * (total_pixels - unexplained_pixels) / total_pixels if total_pixels else None)
    (args.output / "comparison.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    totals = {k: summary[k] for k in ("schemaVersion", "documents", "matrixPairs", "comparedPairs", "passedPairs", "counts",
              "comparedPixels", "rawDifferentPixels", "confirmedBackendPixels", "unexplainedPixels", "exactPixelPercent", "explainedPixelPercent")}
    (args.output / "summary.json").write_text(json.dumps(totals, indent=2) + "\n", encoding="utf-8")
    (args.output / "result.txt").write_text("HTML/CSS rendering pixel comparison\n" + f"Documents: {len(cases)}\nDPI: {selected_dpi}\nCSS viewport: {native_run['width']}x{native_run['height']} (first viewport)\nPairs: {len(results)}\nCompared: {len(pixel_rows)}\nPassed: {summary['passedPairs']}\nChannel tolerance: 0\nResize/case corrections: none\n" + "\n".join(f"{k}: {v}" for k, v in counts.items()) +
        f"\nRaw exact pixel percent (captured pairs): {summary['exactPixelPercent']}\nExplained pixel percent: {summary['explainedPixelPercent']}\nConfirmed backend pixels excluded: {summary['confirmedBackendPixels']}\nMissing captures are failures. 100% achieved: {summary['passedPairs'] == len(results)}\n", encoding="utf-8")
    print(json.dumps(totals, indent=2))
    return 0 if summary["passedPairs"] == len(results) else 1


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--corpus", type=Path, default=Path(__file__).resolve().parent.parent / "rendering-stress")
    p.add_argument("--reference", type=Path, required=True)
    p.add_argument("--native", type=Path, required=True)
    p.add_argument("--software", type=Path)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--case-id", action="append")
    p.add_argument("--dpi", action="append", type=int, choices=(96,144))
    p.add_argument("--no-diff-images", action="store_true",
                   help="Keep full pixel checks and reports without writing additional difference PNGs")
    raise SystemExit(run(p.parse_args()))
