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
    labels = labels_from_config(cfg, "hash_table-gcc-O0")
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
    labels = labels_from_config(cfg, "generated_pthread_mutex-gcc-O0")
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
    labels = labels_from_config(cfg, "binary_search-gcc-O0")
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
    labels = labels_from_config(cfg, "hash_table-gcc-O0")
    assert "HashTable" in labels
    assert "Copy" not in labels
    assert "Memcpy" not in labels


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
    print("algorithm_recovery label tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
