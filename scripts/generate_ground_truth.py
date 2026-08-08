#!/usr/bin/env python3
"""Generate algorithm-recovery ground truth from source label sidecars."""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sources", required=True, help="directory of *.c + *.labels.json")
    ap.add_argument("--manifest", required=True, help="corpus manifest.json from build script")
    ap.add_argument("--out", required=True, help="aggregated ground truth JSON")
    args = ap.parse_args()

    sources = Path(args.sources)
    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    labels_by_key: dict[str, list[str]] = {}

    for label_file in sorted(sources.rglob("*.labels.json")):
        rel = label_file.relative_to(sources)
        key = str(rel)[: -len(".labels.json")].replace("\\", "/")
        stem_key = rel.stem
        data = json.loads(label_file.read_text(encoding="utf-8"))
        labels = list(data.get("algorithms", []))
        labels_by_key[key] = labels
        labels_by_key[stem_key] = labels

    ground: dict[str, list[str]] = {}
    for item in manifest:
        name = item["name"]
        src = item["source"]
        key = src[:-3] if src.endswith(".c") else src
        ground[name] = labels_by_key.get(key, labels_by_key.get(Path(key).name, []))

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(ground, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Wrote {out} ({len(ground)} binaries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
