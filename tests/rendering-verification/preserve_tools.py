"""Verify preserved originals and append a restorable tools/source snapshot."""
import argparse
import datetime
import difflib
import hashlib
import json
import os
from pathlib import Path
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
ALLOWED = {f"TWebFrame2/src/{name}" for name in ("CSS.cpp", "CSS.h", "DOM.cpp", "DOM.h", "Layout.cpp", "Layout.h")}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify_original():
    catalog = json.loads((HERE / "preserved/catalog.json").read_text(encoding="utf-8-sig"))
    archive = HERE / "preserved/inputs-tools-and-engine-before.zip"
    if digest(archive.read_bytes()) != catalog["archiveSha256"]:
        raise ValueError("Original preservation archive changed")
    changed = []
    patches = []
    with zipfile.ZipFile(archive) as z:
        if set(z.namelist()) != set(catalog["files"]):
            raise ValueError("Original archive entry catalog differs")
        for name, expected in catalog["files"].items():
            original = z.read(name)
            if digest(original) != expected:
                raise ValueError("Archive entry hash mismatch: " + name)
            current = (ROOT / name).read_bytes()
            if digest(current) == expected:
                continue
            if name not in ALLOWED:
                raise ValueError("A preserved input/tool or production file outside the allowed common engine changed: " + name)
            changed.append(name)
            patches.extend(difflib.unified_diff(original.decode("utf-8-sig").replace("\r\n", "\n").splitlines(keepends=True),
                current.decode("utf-8-sig").replace("\r\n", "\n").splitlines(keepends=True), fromfile="before/" + name, tofile="after/" + name))
    (HERE / "common-engine-changes.patch").write_text("".join(patches).replace("\r\n", "\n"), encoding="utf-8", newline="\n")
    return catalog, changed


def preserve():
    original, changed = verify_original()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    archive = HERE / "preserved" / ("tools-and-common-engine-" + stamp + ".zip")
    entries = {}
    # Prune generated trees before traversing them: profiles and PNG matrices
    # can contain hundreds of thousands of files that must never be archived.
    for directory, subdirectories, filenames in os.walk(HERE):
        subdirectories[:] = [name for name in subdirectories
                             if name not in {".work", "preserved", "__pycache__"}]
        for filename in filenames:
            path = Path(directory) / filename
            entries[path.relative_to(ROOT).as_posix()] = path.read_bytes()
    for name in ALLOWED:
        entries[name] = (ROOT / name).read_bytes()
    with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in sorted(entries):
            z.writestr(name, entries[name])
    with zipfile.ZipFile(archive) as z:
        for name, contents in entries.items():
            if z.read(name) != contents:
                raise ValueError("New tools archive verification failed")
    sha = digest(archive.read_bytes())
    archive.with_suffix(".zip.sha256").write_text(sha + "  " + archive.name + "\n", encoding="ascii")
    catalog = {"archiveSha256": sha, "originalArchiveSha256": original["archiveSha256"],
               "productionFilesChanged": sorted(changed), "files": {name: digest(contents) for name, contents in sorted(entries.items())}}
    archive.with_suffix(".catalog.json").write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"originalEntriesVerified": len(original["files"]), "preservedInputDocuments": 1000,
                      "productionFilesChanged": sorted(changed), "newArchive": str(archive), "entries": len(entries), "sha256": sha}, indent=2))


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--verify-only", action="store_true")
    args = p.parse_args()
    if args.verify_only:
        original, changed = verify_original()
        print(json.dumps({"verifiedEntries": len(original["files"]), "productionFilesChanged": sorted(changed)}, indent=2))
    else:
        preserve()
