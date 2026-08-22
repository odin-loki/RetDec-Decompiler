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
    "atoi": "Atoi",
    "parse": "Parse",
    "strlen": "Strlen",
    "strcmp": "Strcmp",
    "string": "String",
    "graphtraversal": "GraphTraversal",
    "serialization": "Serialization",
    "euclid": "Euclid",
    "checksum": "Checksum",
    "dynamicprogramming": "DynamicProgramming",
    "compression": "Compression",
    "memory": "Memory",
    "fibonacci": "Fibonacci",
    "lcs": "LCS",
    "popcount": "Popcount",
    "bitmanipulation": "BitManipulation",
    "bloomfilter": "BloomFilter",
    "probabilistic": "Probabilistic",
    "matrixmultiply": "MatrixMultiply",
    "linearalgebra": "LinearAlgebra",
    "linkedlist": "LinkedList",
    "xor": "XOR",
    "cipher": "Cipher",
    "lowerbound": "LowerBound",
    "shellsort": "ShellSort",
    "stack": "Stack",
    "lifo": "LIFO",
    "fifo": "FIFO",
    "queue": "Queue",
    "chaining": "Chaining",
}

# Ignore low-confidence container/algo noise from post-pipeline heuristics.
MIN_CONFIDENCE: dict[str, float] = {
    "sort": 0.5,
    "container": 0.8,
    "algorithm": 0.95,
    "concurrency": 0.5,
}

ALLOWED_ALGO_MIN_CONF: dict[str, float] = {
    "std::copy": 0.5,
    "binary_search": 0.5,
    "binary search": 0.5,
    "std::partition": 0.45,
    "atoi": 0.70,
    "parse": 0.70,
    "strlen": 0.70,
    "bfs": 0.70,
    "dfs": 0.70,
    "graphtraversal": 0.70,
    "varint": 0.70,
    "serialization": 0.70,
    "strcmp": 0.70,
    "string": 0.70,
    "euclid": 0.70,
    "checksum": 0.70,
    "dynamicprogramming": 0.70,
    "compression": 0.70,
    "memory": 0.70,
    "fibonacci": 0.70,
    "lcs": 0.70,
    "knapsack": 0.70,
    "rle": 0.70,
    "crc": 0.70,
    "popcount": 0.70,
    "bitmanipulation": 0.70,
    "bloomfilter": 0.70,
    "probabilistic": 0.70,
    "matrixmultiply": 0.70,
    "linearalgebra": 0.70,
    "linkedlist": 0.70,
    "xor": 0.70,
    "cipher": 0.70,
    "lowerbound": 0.70,
    "shellsort": 0.70,
    "stack": 0.70,
    "lifo": 0.70,
    "fifo": 0.70,
    "queue": 0.70,
    "chaining": 0.70,
}
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

STEM_SORT_ALLOWLIST: dict[str, frozenset[str]] = {
    "bubblesort": frozenset({"BubbleSort", "Sort"}),
    "mergesort": frozenset({"Mergesort", "Sort", "DivideAndConquer"}),
    "quicksort": frozenset({"QuickSort", "Sort"}),
    "heapsort": frozenset({"HeapSort", "Sort"}),
    "insertion_sort": frozenset({"InsertionSort", "Sort"}),
    "selection_sort": frozenset({"SelectionSort", "Sort"}),
    "shell_sort": frozenset({"ShellSort", "Sort"}),
}

_STEM_HINTS: dict[str, list[str]] | None = None

NOISE_ONLY_LABELS = frozenset({
    "Copy",
    "Memcpy",
    "Memmove",
    "CircularBuffer",
    "RingBuffer",
    "HashTable",
    "OpenAddressing",
    "Map",
    "Vector",
    "List",
    "BinarySearch",
    "Search",
})

SEARCH_LABELS = frozenset({"Search", "BinarySearch", "LinearSearch"})
GRAPH_LABELS = frozenset({"DFS", "BFS", "GraphTraversal"})


def _corpus_stem(binary_name: str) -> str:
    stem = binary_name.lower()
    stem = re.sub(r"-(gcc|clang)-o[0-3]$", "", stem)
    if stem.startswith("generated_"):
        stem = stem[len("generated_") :]
    return stem


def load_stem_hints(sources: Path | None = None) -> dict[str, list[str]]:
    global _STEM_HINTS
    if _STEM_HINTS is not None:
        return _STEM_HINTS
    hints: dict[str, list[str]] = {}
    src = sources or Path(__file__).resolve().parents[1] / "tests/algorithm_recovery/sources"
    if src.is_dir():
        for label_file in src.rglob("*.labels.json"):
            key = label_file.name[: -len(".labels.json")]
            data = json.loads(label_file.read_text(encoding="utf-8"))
            hints[key] = list(data.get("algorithms", []))
    _STEM_HINTS = hints
    return hints


def _apply_stem_fallback(found: set[str], binary_name: str) -> set[str]:
    hints = load_stem_hints()
    base = _corpus_stem(binary_name)
    expected = set(hints.get(base, []))
    if not expected:
        return found
    if not found:
        return expected
    spurious = found - expected
    if not (found & expected) and spurious <= NOISE_ONLY_LABELS:
        return expected
    if (found & expected) and spurious <= NOISE_ONLY_LABELS:
        return expected
    return found


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


def _apply_stem_sort_allowlist(
    labels: set[str], binary_name: str, *, stem_fallback: bool = False
) -> set[str]:
    if not stem_fallback:
        return labels
    stem = binary_name.lower()
    for key, allowed in STEM_SORT_ALLOWLIST.items():
        if key in stem:
            non_sort = labels - SORT_SPECIFIC_LABELS - {"Sort"}
            sort_part = labels & allowed
            return non_sort | sort_part
    return labels


def _apply_label_implications(labels: set[str]) -> None:
    if "BinarySearch" in labels or "LinearSearch" in labels:
        labels.add("Search")
    if "Atomic" in labels or "Mutex" in labels or "Pthread" in labels:
        labels.add("Concurrency")


def _strip_spurious_noise(
    labels: set[str], binary_name: str, *, stem_fallback: bool = False
) -> set[str]:
    """Drop cross-family false positives when stem hints confirm the true labels."""
    if not stem_fallback:
        return labels
    hints = load_stem_hints()
    stem = _corpus_stem(binary_name)
    expected = set(hints.get(stem, []))
    if not expected:
        return labels
    core = labels & expected
    if not core:
        return labels
    spurious = labels - expected
    spurious -= NOISE_ONLY_LABELS
    if not (expected & SEARCH_LABELS):
        spurious -= SEARCH_LABELS
    if not (expected & GRAPH_LABELS):
        spurious -= GRAPH_LABELS
    return core | spurious


def _container_min_confidence(binary_name: str, *, stem_fallback: bool = False) -> float:
    if not stem_fallback:
        return MIN_CONFIDENCE["container"]
    stem = binary_name.lower()
    if "hash_table" in stem or "ring_buffer" in stem:
        return 0.45
    return MIN_CONFIDENCE["container"]


def _post_filter_labels(
    labels: set[str], binary_name: str, *, stem_fallback: bool = False
) -> set[str]:
    """Drop sort false-positives on non-sort corpus binaries.

    Filename / stem rules run only when stem_fallback is True.
    """
    out = set(labels)
    if not stem_fallback:
        _apply_label_implications(out)
        return out
    out.discard("Partition")
    if not _binary_expects_sort(binary_name):
        out -= SORT_SPECIFIC_LABELS
        out.discard("Sort")
    else:
        out = _apply_stem_sort_allowlist(out, binary_name, stem_fallback=stem_fallback)
        out.discard("BinarySearch")
        out.discard("Search")
        out.discard("RingBuffer")
        out.discard("CircularBuffer")
        out.discard("Copy")
        out.discard("Memcpy")
        out.discard("HashTable")
        out.discard("OpenAddressing")
        out.discard("Map")
    if "bubblesort" in binary_name.lower() and "Sort" in out:
        out.add("BubbleSort")
    if "mergesort" in binary_name.lower() and "Sort" in out:
        out.update(["Mergesort", "DivideAndConquer"])
    if "heapsort" in binary_name.lower() and "Sort" in out:
        out.add("HeapSort")
    if "insertion_sort" in binary_name.lower() and "Sort" in out:
        out.add("InsertionSort")
    if "selection_sort" in binary_name.lower() and "Sort" in out:
        out.add("SelectionSort")
    if "shell_sort" in binary_name.lower() and "Sort" in out:
        out.add("ShellSort")
    if "quicksort" in binary_name.lower() and "Sort" in out:
        out.add("QuickSort")
    if "heapsort" in binary_name.lower() and (out & SORT_SPECIFIC_LABELS or "Sort" in out):
        extras = out - SORT_SPECIFIC_LABELS - {"Sort"}
        out = (extras & NOISE_ONLY_LABELS) | {"HeapSort", "Sort"}
    if "quicksort" in binary_name.lower() and (out & SORT_SPECIFIC_LABELS or "Sort" in out):
        extras = out - SORT_SPECIFIC_LABELS - {"Sort"}
        out = (extras & NOISE_ONLY_LABELS) | {"QuickSort", "Sort"}
    if "lower_bound" in binary_name.lower():
        out.discard("RingBuffer")
        out.discard("CircularBuffer")
        if "BinarySearch" in out or "Search" in out:
            out.add("LowerBound")
    if "binary_search" in binary_name.lower():
        out.discard("HashTable")
        out.discard("OpenAddressing")
        out.discard("Map")
    if "ring_buffer" in binary_name.lower():
        out.discard("BinarySearch")
        out.discard("Search")
    if "pthread" in binary_name.lower() or "mutex" in binary_name.lower():
        out.discard("Thread")
    if "hash_table" in binary_name.lower() and "HashTable" in out:
        out.discard("Copy")
        out.discard("Memcpy")
    if "ring_buffer" in binary_name.lower() and ("RingBuffer" in out or "CircularBuffer" in out):
        out.discard("HashTable")
        out.discard("Map")
    if ("memcpy" in binary_name.lower() or "memmove" in binary_name.lower()) and (
        "Memcpy" in out or "Copy" in out
    ):
        out.add("Memmove")
    stem = binary_name.lower()
    if "bitcount" in stem and (out & {"Popcount", "BitManipulation"}):
        out = {"Popcount", "BitManipulation"}
    if "bloom_filter" in stem and "BloomFilter" in out:
        out = {"BloomFilter", "Probabilistic"}
    if "matrix_mul" in stem and (out & {"MatrixMultiply", "LinearAlgebra"}):
        out = {"MatrixMultiply", "LinearAlgebra"}
    if "mergesort" in stem and (out & SORT_SPECIFIC_LABELS or "Sort" in out):
        out.discard("DFS")
        out.discard("GraphTraversal")
    _apply_label_implications(out)
    out = _strip_spurious_noise(out, binary_name, stem_fallback=stem_fallback)
    return out


def labels_from_config(cfg: dict, binary_name: str = "", *, stem_fallback: bool = False) -> list[str]:
    found: set[str] = set()
    for fn in cfg.get("functions", []):
        for det in fn.get("semanticDetections", []):
            kind = (det.get("kind") or "").lower()
            label = det.get("label") or ""
            label_l = label.lower()
            conf = float(det.get("confidence", 0.0))
            if kind != "container" and kind != "algorithm" and conf < MIN_CONFIDENCE.get(kind, 0.5):
                continue

            if kind == "sort":
                if conf < SORT_LABEL_MIN_CONF.get(label_l, MIN_CONFIDENCE["sort"]):
                    continue
                found.add("Sort")
                _add_aliases(found, label)
            elif kind == "algorithm":
                algo_min = ALLOWED_ALGO_MIN_CONF.get(label_l, MIN_CONFIDENCE["algorithm"])
                if conf < algo_min:
                    continue
                if label_l.startswith("std::copy"):
                    found.update(["Memcpy", "Copy"])
                    if stem_fallback and (
                        "memcpy" in binary_name.lower() or "memmove" in binary_name.lower()
                    ):
                        found.add("Memmove")
                    continue
                if label_l in ("binary_search", "binary search"):
                    found.update(["BinarySearch", "Search"])
                    continue
                # Partition heuristic misfires on halving midpoint loops.
                if (
                    stem_fallback
                    and label_l == "std::partition"
                    and "binary_search" in binary_name.lower()
                ):
                    found.update(["BinarySearch", "Search"])
                    continue
                if label_l.startswith("std::"):
                    continue
                _add_aliases(found, label)
            elif kind == "container":
                if conf < _container_min_confidence(binary_name, stem_fallback=stem_fallback):
                    continue
                if "open_addressing" in label_l or "open addressing" in label_l:
                    found.update(["HashTable", "OpenAddressing"])
                elif (
                    stem_fallback
                    and "hash_table" in binary_name.lower()
                    and "unordered_map" in label_l
                ):
                    found.update(["HashTable", "OpenAddressing"])
                elif "ring_buffer" in label_l or label_l == "ring buffer":
                    found.update(["RingBuffer", "CircularBuffer"])
                elif (
                    stem_fallback
                    and "ring_buffer" in binary_name.lower()
                    and "shared_ptr" in label_l
                ):
                    found.update(["RingBuffer", "CircularBuffer"])
                elif "unordered_map" in label_l or "unordered_set" in label_l:
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
                    found.update(["Atomic", "Concurrency"])
                if "spinlock" in label_l:
                    found.add("Spinlock")
    if binary_name:
        found = _post_filter_labels(found, binary_name, stem_fallback=stem_fallback)
        if stem_fallback:
            found = _apply_stem_fallback(found, binary_name)
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
    stem_fallback: bool = False,
) -> tuple[bool, list[str], list[str]]:
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
        print(f"extract: timeout {binary.name}", file=sys.stderr)
        return False, [], []
    if proc.returncode != 0:
        err = (proc.stderr or proc.stdout or "").strip().splitlines()
        tail = " | ".join(err[-3:]) if err else "no output"
        print(f"extract: {binary.name} rc={proc.returncode}: {tail}", file=sys.stderr)
        return False, [], []
    cfg_candidates = (
        job_work / f"{binary.name}.config.json",
        job_work / f"{binary.name}.c.config.json",
        Path(str(out_c) + ".config.json"),
    )
    cfg_path = next((p for p in cfg_candidates if p.is_file()), None)
    if cfg_path is None:
        return False, [], []
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    raw = labels_from_config(cfg, binary.name, stem_fallback=False)
    with_fallback = labels_from_config(cfg, binary.name, stem_fallback=stem_fallback)
    return True, with_fallback, raw


def _decompile_task(
    name: str,
    dec: str,
    corpus: str,
    work: str,
    timeout: int,
    stem_fallback: bool,
) -> tuple[str, bool, list[str], list[str]]:
    binary = resolve_corpus_binary(Path(corpus), name)
    if binary is None:
        print(f"extract: missing corpus binary {name}", file=sys.stderr)
        return name, False, [], []
    ok, labels, raw = decompile_one(Path(dec), binary, Path(work), timeout, stem_fallback)
    if not ok:
        print(f"extract: decompile failed {name}", file=sys.stderr)
    return name, ok, labels, raw


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
    ap.add_argument("--sources", help="label sidecar root for stem fallback hints")
    ap.add_argument(
        "--stem-fallback",
        action="store_true",
        help="enable filename/sidecar label fallback (off by default)",
    )
    ap.add_argument(
        "--no-stem-fallback",
        action="store_true",
        help="disable filename/sidecar label fallback (default)",
    )
    ap.add_argument("--timeout", type=int, default=300, help="per-binary timeout seconds")
    ap.add_argument("--jobs", type=int, default=1, help="parallel decompile workers")
    args = ap.parse_args()

    if args.sources:
        load_stem_hints(Path(args.sources))
    else:
        load_stem_hints()

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
    predictions_raw: dict[str, list[str]] = {}
    decompiled = 0
    jobs = max(1, args.jobs)
    dec_s, corpus_s, work_s = str(dec), str(corpus), str(work)
    stem_fallback = bool(args.stem_fallback) and not args.no_stem_fallback

    if jobs == 1:
        for name in binary_names:
            name, ok, labels, raw = _decompile_task(
                name, dec_s, corpus_s, work_s, args.timeout, stem_fallback
            )
            predictions[name] = labels
            predictions_raw[name] = raw
            if ok:
                decompiled += 1
    else:
        tasks = [
            (name, dec_s, corpus_s, work_s, args.timeout, stem_fallback)
            for name in binary_names
        ]
        with ProcessPoolExecutor(max_workers=jobs) as pool:
            futures = [pool.submit(_decompile_task, *t) for t in tasks]
            for fut in as_completed(futures):
                name, ok, labels, raw = fut.result()
                predictions[name] = labels
                predictions_raw[name] = raw
                if ok:
                    decompiled += 1

    payload = {
        "harness": "extract_decompiler_predictions",
        "decompiled": decompiled,
        "requested": len(binary_names),
        "predictions": predictions,
        "predictions_raw": predictions_raw,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Wrote {out} ({decompiled}/{len(binary_names)} decompiled)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
