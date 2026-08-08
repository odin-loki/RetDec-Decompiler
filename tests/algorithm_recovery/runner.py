#!/usr/bin/env python3
"""Algorithm-recovery metric (precision/recall/F1 per class, per optimisation level)."""
from __future__ import annotations

import argparse
import json
import re
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


def opt_from_name(name: str) -> str:
    m = re.search(r"-(O\d)$", name, re.IGNORECASE)
    return m.group(1).upper() if m else "unknown"


def mean_f1(per_binary: dict) -> float:
    f1_values = [v.get("f1", 0.0) for v in per_binary.values()]
    return sum(f1_values) / len(f1_values) if f1_values else 0.0


def summarize_per_opt(per_binary: dict, eval_names: list[str]) -> dict:
    by_opt: dict[str, list[str]] = {}
    for name in eval_names:
        by_opt.setdefault(opt_from_name(name), []).append(name)
    out = {}
    for opt, names in sorted(by_opt.items()):
        subset = {n: per_binary[n] for n in names if n in per_binary}
        out[opt] = {
            "binaries": len(subset),
            "mean_f1": mean_f1(subset),
        }
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--predictions", required=True, help="JSON map binary -> [classes]")
    ap.add_argument("--ground-truth", required=True, help="JSON map binary -> [classes]")
    ap.add_argument("--out", default="results/algorithm_recovery.json")
    args = ap.parse_args()

    truth = json.loads(Path(args.ground_truth).read_text(encoding="utf-8"))
    raw = json.loads(Path(args.predictions).read_text(encoding="utf-8"))
    pred_raw_map = raw.get("predictions_raw") if isinstance(raw, dict) else None
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

    per_binary_raw = {}
    if pred_raw_map:
        for name in eval_names:
            p = set(pred_raw_map.get(name, []))
            e = set(truth.get(name, []))
            per_binary_raw[name] = score(p, e)

    out = {
        "harness": "algorithm_recovery",
        "summary": {
            "binaries": len(per_binary),
            "mean_f1": mean_f1(per_binary),
            "mean_f1_raw": mean_f1(per_binary_raw) if per_binary_raw else None,
            "decompiled": decompiled,
        },
        "per_opt": summarize_per_opt(per_binary, eval_names),
        "per_binary": per_binary,
        "per_class": per_class,
    }
    if per_binary_raw:
        out["per_opt_raw"] = summarize_per_opt(per_binary_raw, eval_names)
        out["per_binary_raw"] = per_binary_raw

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
