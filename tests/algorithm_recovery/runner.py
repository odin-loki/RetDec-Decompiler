#!/usr/bin/env python3
"""Algorithm-recovery metric scaffold (precision/recall/F1 per class)."""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def score(predicted: set[str], expected: set[str]) -> dict:
    tp = len(predicted & expected)
    fp = len(predicted - expected)
    fn = len(expected - predicted)
    precision = tp / (tp + fp) if (tp + fp) else 0.0
    recall = tp / (tp + fn) if (tp + fn) else 0.0
    f1 = (2 * precision * recall / (precision + recall)) if (precision + recall) else 0.0
    return {
        "tp": tp,
        "fp": fp,
        "fn": fn,
        "precision": precision,
        "recall": recall,
        "f1": f1,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--predictions", required=True, help="JSON map binary -> [classes]")
    ap.add_argument("--ground-truth", required=True, help="JSON map binary -> [classes]")
    ap.add_argument("--out", default="results/algorithm_recovery.json")
    args = ap.parse_args()

    truth = json.loads(Path(args.ground_truth).read_text(encoding="utf-8"))
    raw = json.loads(Path(args.predictions).read_text(encoding="utf-8"))
    if "predictions" in raw:
        decompiled = raw.get("decompiled")
        pred = raw["predictions"]
        eval_names = sorted(pred.keys())
    else:
        decompiled = None
        pred = raw
        eval_names = sorted(set(pred) | set(truth))

    per_binary = {}
    classes: set[str] = set()
    for name in eval_names:
        classes.update(truth.get(name, []))
        classes.update(pred.get(name, []))

    for name in eval_names:
        p = set(pred.get(name, []))
        e = set(truth.get(name, []))
        per_binary[name] = score(p, e)

    per_class = {}
    for cls in sorted(classes):
        p = {b for b in eval_names if cls in pred.get(b, [])}
        e = {b for b in eval_names if cls in truth.get(b, [])}
        per_class[cls] = score(p, e)

    f1_values = [v.get("f1", 0.0) for v in per_binary.values()]
    mean_f1 = sum(f1_values) / len(f1_values) if f1_values else 0.0

    out = {
        "harness": "algorithm_recovery",
        "summary": {
            "binaries": len(per_binary),
            "mean_f1": mean_f1,
            "decompiled": decompiled,
        },
        "per_binary": per_binary,
        "per_class": per_class,
    }
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
