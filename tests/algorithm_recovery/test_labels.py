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


def main() -> int:
    test_sort_detection()
    test_hash_table_container()
    test_concurrency()
    print("algorithm_recovery label tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
