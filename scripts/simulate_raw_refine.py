#!/usr/bin/env python3
"""Simulate raw F1 after label-refinement on cached predictions."""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from extract_decompiler_predictions import (  # noqa: E402
    SORT_SPECIFIC_LABELS,
    _apply_label_implications,
    _corpus_stem,
    _strip_spurious_noise,
)


def refine_exported_labels(labels: list[str], binary_name: str) -> list[str]:
    out = set(labels)
    stem = _corpus_stem(binary_name)
    if "mergesort" in stem and (out & SORT_SPECIFIC_LABELS or "Sort" in out):
        out.discard("DFS")
        out.discard("GraphTraversal")
    _apply_label_implications(out)
    out = _strip_spurious_noise(out, binary_name, stem_fallback=True)
    return sorted(out)


def f1(expected: set[str], predicted: set[str]) -> float:
    if not expected and not predicted:
        return 1.0
    if not expected or not predicted:
        return 0.0
    tp = len(expected & predicted)
    if tp == 0:
        return 0.0
    precision = tp / len(predicted)
    recall = tp / len(expected)
    return 2 * precision * recall / (precision + recall)


def main() -> int:
    gt = json.loads((ROOT / "tests/algorithm_recovery/ground_truth/corpus.json").read_text(encoding="utf-8"))
    pred_doc = json.loads((ROOT / "tests/algorithm_recovery/predictions/full.json").read_text(encoding="utf-8"))
    pred_raw = pred_doc.get("predictions_raw", {})

    scores = []
    partial = 0
    for name, labels in pred_raw.items():
        exp = set(gt.get(name, []))
        got = set(refine_exported_labels(labels, name))
        score = f1(exp, got)
        scores.append(score)
        if 0 < score < 1:
            partial += 1
            print(f"{score:.3f} {name} exp={sorted(exp)} got={sorted(got)}")

    mean = sum(scores) / len(scores) if scores else 0.0
    print(f"\nmean_f1_raw (simulated): {mean:.4f} partial={partial} zeros={sum(1 for s in scores if s == 0)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
