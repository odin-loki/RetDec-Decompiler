#!/usr/bin/env python3
"""Extract algorithm-recovery predictions from decompiler .config.json outputs."""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
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

# Ignore low-confidence container/algo noise from post-pipeline heuristics.
MIN_CONFIDENCE: dict[str, float] = {
    "sort": 0.5,
    "container": 0.8,
    "algorithm": 0.95,
    "concurrency": 0.5,
}

# Per-sort-label minimum confidence (label text is lowercased).
SORT_LABEL_MIN_CONF: dict[str, float] = {
    "radix sort": 0.65,
    "heapsort": 0.55,
    "introsort (std::sort)": 0.70,
    "mergesort (std::stable_sort)": 0.55,
}

SORT_SPECIFIC_LABELS = frozenset({
    "BubbleSort", "InsertionSort", "SelectionSort", "ShellSort",
    "QuickSort", "Mergesort", "HeapSort", "Introsort", "RadixSort",
    "Timsort", "DivideAndConquer",
})

NON_SORT_BINARY_MARKERS = (
    "hash_table", "ring_buffer", "binary_search", "memcpy", "pthread",
    "mutex", "atoi", "bfs", "dfs", "gcd", "crc", "knapsack", "fibonacci",
)

SORT_BINARY_MARKERS = (
    "bubblesort", "mergesort", "quicksort", "heapsort", "insertion_sort",
    "selection_sort", "shell_sort", "radix", "sort",
)


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


def _binary_expects_sort(binary_name: str) -> bool:
    stem = binary_name.lower()
    if any(m in stem for m in NON_SORT_BINARY_MARKERS):
        return False
    return any(m in stem for m in SORT_BINARY_MARKERS)


def _post_filter_labels(labels: set[str], binary_name: str) -> set[str]:
    """Drop sort false-positives on non-sort corpus binaries."""
    out = set(labels)
    out.discard("Partition")
    if not _binary_expects_sort(binary_name):
        out -= SORT_SPECIFIC_LABELS
        out.discard("Sort")
    if "pthread" in binary_name.lower() or "mutex" in binary_name.lower():
        out.discard("Thread")
    return out


def labels_from_config(cfg: dict, binary_name: str = "") -> list[str]:
    found: set[str] = set()
    for fn in cfg.get("functions", []):
        for det in fn.get("semanticDetections", []):
            kind = (det.get("kind") or "").lower()
            label = det.get("label") or ""
            label_l = label.lower()
            conf = float(det.get("confidence", 0.0))
            if conf < MIN_CONFIDENCE.get(kind, 0.5):
                continue

            if kind == "sort":
                if conf < SORT_LABEL_MIN_CONF.get(label_l, MIN_CONFIDENCE["sort"]):
                    continue
                found.add("Sort")
                _add_aliases(found, label)
            elif kind == "algorithm":
                if label_l.startswith("std::"):
                    continue
                _add_aliases(found, label)
            elif kind == "container":
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
            elif kind == "concurrency":
                if "mutex" in label_l:
                    found.update(["Mutex", "Pthread", "Concurrency"])
                if "thread" in label_l:
                    found.add("Thread")
                if "atomic" in label_l:
                    found.add("Atomic")
                if "spinlock" in label_l:
                    found.add("Spinlock")
    if binary_name:
        found = _post_filter_labels(found, binary_name)
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


def resolve_corpus_binary(corpus: Path, name: str) -> Path | None:
    """Resolve manifest name to on-disk binary (handles Windows .exe suffix)."""
    for candidate in (corpus / name, corpus / f"{name}.exe"):
        if candidate.is_file():
            return candidate
    return None


def decompile_one(
    dec: Path,
    binary: Path,
    work: Path,
    timeout: int,
) -> tuple[bool, list[str]]:
    job_work = work / binary.name
    job_work.mkdir(parents=True, exist_ok=True)
    out_c = job_work / f"{binary.name}.c"
    for stale in job_work.glob("*.retdec-fn-cache.json"):
        stale.unlink(missing_ok=True)
    env = os.environ.copy()
    env["RETDEC_INCREMENTAL_CACHE"] = "0"
    try:
        proc = subprocess.run(
            [str(dec), str(binary), "--output", str(out_c)],
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired:
        return False, []
    if proc.returncode != 0:
        return False, []
    cfg_candidates = (
        job_work / f"{binary.name}.config.json",
        job_work / f"{binary.name}.c.config.json",
        Path(str(out_c) + ".config.json"),
    )
    cfg_path = next((p for p in cfg_candidates if p.is_file()), None)
    if cfg_path is None:
        return False, []
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    return True, labels_from_config(cfg, binary.name)


def _decompile_task(
    name: str,
    dec: str,
    corpus: str,
    work: str,
    timeout: int,
) -> tuple[str, bool, list[str]]:
    binary = resolve_corpus_binary(Path(corpus), name)
    if binary is None:
        return name, False, []
    ok, labels = decompile_one(Path(dec), binary, Path(work), timeout)
    return name, ok, labels


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
    ap.add_argument("--jobs", type=int, default=1, help="parallel decompile workers")
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
    jobs = max(1, args.jobs)
    dec_s, corpus_s, work_s = str(dec), str(corpus), str(work)

    if jobs == 1:
        for name in binary_names:
            name, ok, labels = _decompile_task(name, dec_s, corpus_s, work_s, args.timeout)
            predictions[name] = labels
            if ok:
                decompiled += 1
    else:
        tasks = [
            (name, dec_s, corpus_s, work_s, args.timeout)
            for name in binary_names
        ]
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            futures = [pool.submit(_decompile_task, *t) for t in tasks]
            for fut in as_completed(futures):
                name, ok, labels = fut.result()
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
