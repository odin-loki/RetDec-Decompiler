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


def main() -> int:
    test_sort_detection()
    test_hash_table_container()
    test_concurrency()
    test_low_confidence_container_ignored()
    test_stl_algorithm_noise_filtered()
    test_non_sort_binary_filters_sort_labels()
    test_mutex_adds_pthread_labels()
    print("algorithm_recovery label tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
