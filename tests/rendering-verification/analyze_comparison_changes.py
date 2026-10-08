"""Summarize changes between two complete, comparable pixel matrices."""
import argparse
import json
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def analyze(before, after, output):
    first, second = read(before / "comparison.json"), read(after / "comparison.json")
    for field in ("schemaVersion", "manifestSha256", "documents", "matrixPairs", "dpi", "cssViewport", "channelTolerance", "resize"):
        if first[field] != second[field]:
            raise ValueError("Matrix provenance differs: " + field)
    previous = {(row["id"], row["dpi"]): row for row in first["results"]}
    current = {(row["id"], row["dpi"]): row for row in second["results"]}
    if len(previous) != first["matrixPairs"] or previous.keys() != current.keys():
        raise ValueError("Matrix pairs are missing or duplicated")
    changes = []
    for key, row in current.items():
        old = previous[key]
        if "unexplainedPixels" not in row or "unexplainedPixels" not in old:
            continue
        changes.append({"id": key[0], "dpi": key[1], "before": old["unexplainedPixels"],
                        "after": row["unexplainedPixels"], "change": row["unexplainedPixels"] - old["unexplainedPixels"]})
    summary = {"before": str(before.resolve()), "after": str(after.resolve()), "comparedPairs": len(changes),
               "improved": sum(row["change"] < 0 for row in changes),
               "unchanged": sum(row["change"] == 0 for row in changes),
               "worse": sum(row["change"] > 0 for row in changes),
               "unexplainedPixelChange": sum(row["change"] for row in changes),
               "beforeExplainedPixelPercent": first["explainedPixelPercent"],
               "afterExplainedPixelPercent": second["explainedPixelPercent"],
               "mostImproved": sorted((row for row in changes if row["change"] < 0), key=lambda row: row["change"])[:10],
               "mostWorsened": sorted((row for row in changes if row["change"] > 0), key=lambda row: row["change"], reverse=True)[:10],
               "perDpi": {str(dpi): {"compared": sum(row["dpi"] == dpi for row in changes),
                                     "unexplainedPixelChange": sum(row["change"] for row in changes if row["dpi"] == dpi)}
                          for dpi in second["dpi"]}}
    output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in summary.items() if key not in ("mostImproved", "mostWorsened")}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    analyze(args.before, args.after, args.output)
