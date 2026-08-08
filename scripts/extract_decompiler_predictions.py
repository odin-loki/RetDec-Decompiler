#!/usr/bin/env python3
"""Extract algorithm-recovery predictions from decompiler .config.json outputs."""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

LABEL_MAP = {
    "introsort": "Introsort",
    "mergesort": "Mergesort",
    "heapsort": "HeapSort",
    "quicksort": "QuickSort",
    "bubblesort": "BubbleSort",
    "sort": "Sort",
    "transform": "Transform",
    "accumulate": "Accumulate",
    "find": "Find",
    "partition": "Partition",
    "vector": "Vector",
    "list": "List",
    "map": "Map",
    "mutex": "Mutex",
    "thread": "Thread",
    "atomic": "Atomic",
}


def labels_from_config(cfg: dict) -> list[str]:
    found: set[str] = set()
    for fn in cfg.get("functions", []):
        for det in fn.get("semanticDetections", []):
            kind = (det.get("kind") or "").lower()
            label = (det.get("label") or "").lower()
            if kind == "sort":
                found.add("Sort")
            if kind == "algorithm":
                found.add("Algorithm")
            for token in label.replace("::", " ").replace("_", " ").split():
                key = token.lower()
                if key in LABEL_MAP:
                    found.add(LABEL_MAP[key])
                elif key and key[0].isalpha():
                    found.add(token.title())
    return sorted(found)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--decompiler", required=True)
    ap.add_argument("--corpus", required=True, help="corpus directory of binaries")
    ap.add_argument("--out", required=True)
    ap.add_argument("--work", default="build/prediction-work")
    args = ap.parse_args()

    dec = Path(args.decompiler)
    corpus = Path(args.corpus)
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)

    predictions: dict[str, list[str]] = {}
    for binary in sorted(corpus.iterdir()):
        if not binary.is_file() or binary.name.endswith(".json"):
            continue
        out_c = work / f"{binary.name}.c"
        cfg_path = work / f"{binary.name}.c.config.json"
        proc = subprocess.run(
            [str(dec), str(binary), "--output", str(out_c)],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            predictions[binary.name] = []
            continue
        if not cfg_path.is_file():
            cfg_path = Path(str(out_c) + ".config.json")
        if not cfg_path.is_file():
            predictions[binary.name] = []
            continue
        cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
        predictions[binary.name] = labels_from_config(cfg)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(predictions, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Wrote {out} ({len(predictions)} binaries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
