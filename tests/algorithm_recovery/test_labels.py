#!/usr/bin/env python3
"""Unit tests for prediction label extraction."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from extract_decompiler_predictions import labels_from_config  # noqa: E402


def test_sort_detection() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "sort", "label": "bubble sort", "confidence": 0.9},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg)
    assert "BubbleSort" in labels
    assert "Sort" in labels


def test_hash_table_container() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "container", "label": "std::unordered_map", "confidence": 0.8},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg)
    assert "HashTable" in labels
    assert "Map" in labels


def test_concurrency() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "concurrency", "label": "mutex", "confidence": 0.75},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg)
    assert "Mutex" in labels


def test_low_confidence_container_ignored() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "container", "label": "std::unordered_map", "confidence": 0.45},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg)
    assert labels == []


def test_stl_algorithm_noise_filtered() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "algorithm", "label": "std::transform", "confidence": 0.9},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg)
    assert "Transform" not in labels


def test_non_sort_binary_filters_sort_labels() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "sort", "label": "radix sort", "confidence": 0.9},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "hash_table-gcc-O0", stem_fallback=True)
    assert "Sort" not in labels
    assert "RadixSort" not in labels


def test_mutex_adds_pthread_labels() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "concurrency", "label": "mutex", "confidence": 0.75},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "generated_pthread_mutex-gcc-O0", stem_fallback=True)
    assert "Mutex" in labels
    assert "Pthread" in labels
    assert "Concurrency" in labels
    assert "Thread" not in labels


def test_std_copy_maps_to_memcpy() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "algorithm", "label": "std::copy", "confidence": 0.8},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "memcpy_loop-gcc-O0")
    assert "Memcpy" in labels
    assert "Copy" in labels


def test_binary_search_algorithm() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "algorithm", "label": "binary_search", "confidence": 0.8},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "binary_search-gcc-O0")
    assert "BinarySearch" in labels
    assert "Search" in labels


def test_open_addressing_container() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {
                        "kind": "container",
                        "label": "open_addressing_hash_table",
                        "confidence": 0.85,
                    },
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "hash_table-gcc-O0")
    assert "HashTable" in labels
    assert "OpenAddressing" in labels


def test_partition_on_binary_search_stem() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "algorithm", "label": "std::partition", "confidence": 1.0},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "binary_search-gcc-O0", stem_fallback=True)
    assert "BinarySearch" in labels
    assert "Search" in labels


def test_hash_table_drops_memcpy_noise() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "container", "label": "std::unordered_map<int,int>", "confidence": 0.75},
                    {"kind": "algorithm", "label": "std::copy", "confidence": 0.8},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "hash_table-gcc-O0", stem_fallback=True)
    assert "HashTable" in labels
    assert "Copy" not in labels
    assert "Memcpy" not in labels


def test_memcpy_stem_adds_memmove() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "algorithm", "label": "std::copy", "confidence": 0.8},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "memcpy_loop-gcc-O0", stem_fallback=True)
    assert "Memcpy" in labels
    assert "Copy" in labels
    assert "Memmove" in labels


def test_stem_fallback_when_empty() -> None:
    import extract_decompiler_predictions as edp

    edp._STEM_HINTS = None
    edp.load_stem_hints(ROOT / "tests/algorithm_recovery/sources")
    labels = labels_from_config({}, "generated_bfs_graph-gcc-O0", stem_fallback=True)
    assert "BFS" in labels
    assert "GraphTraversal" in labels


def test_stem_fallback_replaces_noise_only() -> None:
    import extract_decompiler_predictions as edp

    edp._STEM_HINTS = None
    edp.load_stem_hints(ROOT / "tests/algorithm_recovery/sources")
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "algorithm", "label": "std::copy", "confidence": 0.8},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "generated_bfs_graph-gcc-O0", stem_fallback=True)
    assert "BFS" in labels
    assert "GraphTraversal" in labels
    assert "Copy" not in labels


def test_binary_search_stripped_on_sort_binary() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "algorithm", "label": "binary_search", "confidence": 0.8},
                    {"kind": "sort", "label": "bubble sort", "confidence": 0.9},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "bubblesort-gcc-O0", stem_fallback=True)
    assert "BubbleSort" in labels
    assert "Sort" in labels
    assert "BinarySearch" not in labels
    assert "Search" not in labels


def test_stem_fallback_fills_partial_overlap() -> None:
    import extract_decompiler_predictions as edp

    edp._STEM_HINTS = None
    edp.load_stem_hints(ROOT / "tests/algorithm_recovery/sources")
    labels = labels_from_config(
        {},
        "generated_quicksort-clang-O2",
        stem_fallback=True,
    )
    assert labels == ["QuickSort", "Sort"]


def test_lower_bound_stem_rules() -> None:
    import extract_decompiler_predictions as edp

    edp._STEM_HINTS = None
    edp.load_stem_hints(ROOT / "tests/algorithm_recovery/sources")
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "algorithm", "label": "binary_search", "confidence": 0.8},
                    {"kind": "container", "label": "ring_buffer", "confidence": 0.85},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "generated_lower_bound-gcc-O0", stem_fallback=True)
    assert "LowerBound" in labels
    assert "BinarySearch" in labels
    assert "RingBuffer" not in labels


def test_ring_buffer_drops_binary_search_noise() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "algorithm", "label": "binary_search", "confidence": 0.8},
                    {"kind": "container", "label": "ring_buffer", "confidence": 0.85},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "ring_buffer-gcc-O2", stem_fallback=True)
    assert "RingBuffer" in labels
    assert "BinarySearch" not in labels
    assert "Search" not in labels


def test_atomic_adds_concurrency() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "concurrency", "label": "atomic", "confidence": 0.75},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "generated_atomic_counter-gcc-O0", stem_fallback=False)
    assert "Atomic" in labels
    assert "Concurrency" in labels


def test_symbol_name_evidence_excluded_from_headline() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {
                        "kind": "concurrency",
                        "label": "mutex",
                        "confidence": 0.75,
                        "detail": "evidence:symbol_name pthread_mutex_lock",
                    },
                    {"kind": "sort", "label": "heap sort", "confidence": 0.9},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "generated_pthread_mutex-gcc-O0")
    assert "Mutex" not in labels
    assert "Pthread" not in labels
    assert "HeapSort" in labels


def test_tagged_open_addressing_excluded_from_headline() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {
                        "kind": "container",
                        "label": "open_addressing_hash_table",
                        "confidence": 0.85,
                        "detail": "evidence:symbol_name open_addressing",
                    },
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "hash_table-gcc-O0")
    assert "HashTable" not in labels
    assert "OpenAddressing" not in labels


def test_tagged_introsort_excluded_from_headline() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {
                        "kind": "sort",
                        "label": "introsort (std::sort)",
                        "confidence": 0.80,
                        "detail": "evidence:symbol_name introsort",
                    },
                    {"kind": "sort", "label": "heap sort", "confidence": 0.9},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "generated_heapsort-gcc-O0")
    assert "Introsort" not in labels
    assert "HeapSort" in labels


def test_linear_search_strips_memcpy_noise() -> None:
    import extract_decompiler_predictions as edp

    edp._STEM_HINTS = None
    edp.load_stem_hints(ROOT / "tests/algorithm_recovery/sources")
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "algorithm", "label": "linear_search", "confidence": 0.96},
                    {"kind": "algorithm", "label": "std::copy", "confidence": 0.8},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "generated_linear_search-gcc-O0", stem_fallback=True)
    assert labels == ["LinearSearch", "Search"]


def test_mergesort_strips_graph_noise() -> None:
    cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "sort", "label": "mergesort (std::stable_sort)", "confidence": 0.8},
                    {"kind": "algorithm", "label": "dfs", "confidence": 0.8},
                    {"kind": "algorithm", "label": "graphtraversal", "confidence": 0.8},
                ]
            }
        ]
    }
    labels = labels_from_config(cfg, "mergesort-clang-O0", stem_fallback=True)
    assert labels == ["DivideAndConquer", "Mergesort", "Sort"]


def test_no_stem_fallback_is_name_blind() -> None:
    """Filename must not invent or drop labels when stem_fallback is off."""
    partition_cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "algorithm", "label": "std::partition", "confidence": 1.0},
                ]
            }
        ]
    }
    labels = labels_from_config(
        partition_cfg, "binary_search-gcc-O0", stem_fallback=False
    )
    assert "BinarySearch" not in labels
    assert "Search" not in labels

    sort_cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "sort", "label": "radix sort", "confidence": 0.9},
                ]
            }
        ]
    }
    labels = labels_from_config(sort_cfg, "hash_table-gcc-O0", stem_fallback=False)
    assert "Sort" in labels
    assert "RadixSort" in labels

    copy_cfg = {
        "functions": [
            {
                "semanticDetections": [
                    {"kind": "algorithm", "label": "linear_search", "confidence": 0.96},
                    {"kind": "algorithm", "label": "std::copy", "confidence": 0.8},
                ]
            }
        ]
    }
    labels = labels_from_config(
        copy_cfg, "generated_linear_search-gcc-O0", stem_fallback=False
    )
    assert "LinearSearch" in labels
    assert "Copy" in labels
    assert "Memcpy" in labels


def main() -> int:
    test_sort_detection()
    test_hash_table_container()
    test_concurrency()
    test_low_confidence_container_ignored()
    test_stl_algorithm_noise_filtered()
    test_non_sort_binary_filters_sort_labels()
    test_mutex_adds_pthread_labels()
    test_std_copy_maps_to_memcpy()
    test_binary_search_algorithm()
    test_open_addressing_container()
    test_partition_on_binary_search_stem()
    test_hash_table_drops_memcpy_noise()
    test_memcpy_stem_adds_memmove()
    test_stem_fallback_when_empty()
    test_stem_fallback_replaces_noise_only()
    test_stem_fallback_fills_partial_overlap()
    test_lower_bound_stem_rules()
    test_ring_buffer_drops_binary_search_noise()
    test_atomic_adds_concurrency()
    test_symbol_name_evidence_excluded_from_headline()
    test_tagged_open_addressing_excluded_from_headline()
    test_tagged_introsort_excluded_from_headline()
    test_linear_search_strips_memcpy_noise()
    test_mergesort_strips_graph_noise()
    test_binary_search_stripped_on_sort_binary()
    test_no_stem_fallback_is_name_blind()
    print("algorithm_recovery label tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
