#!/usr/bin/env python3
"""Extract algorithm-recovery predictions from decompiler .config.json outputs."""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

# Hand-written corpus sources at gcc-O0 — fast CI smoke for live F1.
CI_CORE_NAMES = [
    "bubblesort-gcc-O0",
    "mergesort-gcc-O0",
    "hash_table-gcc-O0",
    "ring_buffer-gcc-O0",
    "binary_search-gcc-O0",
    "memcpy_loop-gcc-O0",
    "generated_quicksort-gcc-O0",
    "generated_heapsort-gcc-O0",
    "generated_pthread_mutex-gcc-O0",
]

SORT_ALIASES: dict[str, list[str]] = {
    "bubble sort": ["BubbleSort", "Sort"],
    "insertion sort": ["InsertionSort", "Sort"],
    "selection sort": ["SelectionSort", "Sort"],
    "shell sort": ["ShellSort", "Sort"],
    "quicksort": ["QuickSort", "Sort"],
    "mergesort": ["Mergesort", "Sort", "DivideAndConquer"],
    "heapsort": ["HeapSort", "Sort"],
    "introsort": ["Introsort", "Sort"],
    "radix sort": ["RadixSort", "Sort"],
    "timsort": ["Timsort", "Sort"],
}

TOKEN_MAP = {
    "introsort": "Introsort",
    "mergesort": "Mergesort",
    "heapsort": "HeapSort",
    "quicksort": "QuickSort",
    "bubblesort": "BubbleSort",
    "insertionsort": "InsertionSort",
    "selectionsort": "SelectionSort",
    "shellsort": "ShellSort",
    "sort": "Sort",
    "transform": "Transform",
    "accumulate": "Accumulate",
    "find": "Find",
    "partition": "Partition",
    "copy": "Copy",
    "fill": "Fill",
    "memcpy": "Memcpy",
    "memset": "Memset",
    "binarysearch": "BinarySearch",
    "linearsearch": "LinearSearch",
    "hashtable": "HashTable",
    "ringbuffer": "RingBuffer",
    "circularbuffer": "CircularBuffer",
    "mutex": "Mutex",
    "thread": "Thread",
    "atomic": "Atomic",
    "dfs": "DFS",
    "bfs": "BFS",
    "knapsack": "Knapsack",
    "gcd": "GCD",
    "crc": "CRC",
    "rle": "RLE",
    "varint": "Varint",
}


def _norm(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", text.lower()).strip()


def _add_aliases(found: set[str], text: str) -> None:
    norm = _norm(text)
    if norm in SORT_ALIASES:
        found.update(SORT_ALIASES[norm])
    compact = norm.replace(" ", "")
    if compact in TOKEN_MAP:
        found.add(TOKEN_MAP[compact])
    for token in norm.split():
        if token in TOKEN_MAP:
            found.add(TOKEN_MAP[token])


def labels_from_config(cfg: dict) -> list[str]:
    found: set[str] = set()
    for fn in cfg.get("functions", []):
        for det in fn.get("semanticDetections", []):
            kind = (det.get("kind") or "").lower()
            label = det.get("label") or ""
            label_l = label.lower()

            if kind == "sort":
                found.add("Sort")
                _add_aliases(found, label)

            if kind == "algorithm":
                found.add("Algorithm")
                _add_aliases(found, label)

            if kind == "container":
                if "unordered_map" in label_l or "unordered_set" in label_l:
                    found.update(["HashTable", "Map"])
                elif "map" in label_l:
                    found.add("Map")
                elif "vector" in label_l:
                    found.add("Vector")
                elif "list" in label_l:
                    found.add("List")
                elif "deque" in label_l or "ring" in label_l:
                    found.update(["RingBuffer", "CircularBuffer"])
                _add_aliases(found, label)

            if kind == "concurrency":
                if "mutex" in label_l:
                    found.add("Mutex")
                if "thread" in label_l:
                    found.add("Thread")
                if "atomic" in label_l:
                    found.add("Atomic")
                if "spinlock" in label_l:
                    found.add("Spinlock")
                _add_aliases(found, label)

            _add_aliases(found, label)
    return sorted(found)


def load_binary_names(
    corpus: Path,
    manifest: Path | None,
    ci_core: bool,
    names: list[str] | None,
    limit: int | None,
) -> list[str]:
    if ci_core:
        return list(CI_CORE_NAMES)

    if names:
        return names

    if manifest and manifest.is_file():
        items = json.loads(manifest.read_text(encoding="utf-8"))
        selected = [item["name"] for item in items if "name" in item]
        if limit is not None:
            return selected[:limit]
        return selected

    selected = sorted(
        p.name
        for p in corpus.iterdir()
        if p.is_file() and not p.name.endswith(".json")
    )
    if limit is not None:
        return selected[:limit]
    return selected


def decompile_one(
    dec: Path,
    binary: Path,
    work: Path,
    timeout: int,
) -> tuple[bool, list[str]]:
    out_c = work / f"{binary.name}.c"
    cfg_path = work / f"{binary.name}.c.config.json"
    try:
        proc = subprocess.run(
            [str(dec), str(binary), "--output", str(out_c)],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return False, []
    if proc.returncode != 0:
        return False, []
    if not cfg_path.is_file():
        cfg_path = Path(str(out_c) + ".config.json")
    if not cfg_path.is_file():
        return False, []
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    return True, labels_from_config(cfg)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--decompiler", required=True)
    ap.add_argument("--corpus", required=True, help="corpus directory of binaries")
    ap.add_argument("--out", required=True)
    ap.add_argument("--manifest", help="corpus manifest.json (preferred binary list)")
    ap.add_argument("--work", default="build/prediction-work")
    ap.add_argument("--limit", type=int, help="decompile at most N binaries")
    ap.add_argument("--names", help="comma-separated binary names to decompile")
    ap.add_argument("--ci-core", action="store_true", help="CI smoke subset (9 binaries)")
    ap.add_argument("--timeout", type=int, default=300, help="per-binary timeout seconds")
    args = ap.parse_args()

    dec = Path(args.decompiler)
    corpus = Path(args.corpus)
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)

    names_arg = [n.strip() for n in args.names.split(",") if n.strip()] if args.names else None
    manifest = Path(args.manifest) if args.manifest else corpus / "manifest.json"
    binary_names = load_binary_names(
        corpus, manifest if manifest.is_file() else None,
        args.ci_core, names_arg, args.limit,
    )

    predictions: dict[str, list[str]] = {}
    decompiled = 0
    for name in binary_names:
        binary = corpus / name
        if not binary.is_file():
            predictions[name] = []
            continue
        ok, labels = decompile_one(dec, binary, work, args.timeout)
        predictions[name] = labels
        if ok:
            decompiled += 1

    payload = {
        "harness": "extract_decompiler_predictions",
        "decompiled": decompiled,
        "requested": len(binary_names),
        "predictions": predictions,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Wrote {out} ({decompiled}/{len(binary_names)} decompiled)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
