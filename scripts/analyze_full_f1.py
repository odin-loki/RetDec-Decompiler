#!/usr/bin/env python3
"""Analyze algorithm-recovery F1 results (with and without stem fallback)."""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def summarize(per_binary: dict[str, dict]) -> tuple[int, int, int]:
    zeros = [k for k, v in per_binary.items() if v.get("f1", 0) == 0]
    partial = [k for k, v in per_binary.items() if 0 < v.get("f1", 0) < 1]
    perfect = [k for k, v in per_binary.items() if v.get("f1", 0) == 1]
    return len(zeros), len(partial), len(perfect)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results/algorithm-recovery-full.json")
    ap.add_argument("--predictions", default="tests/algorithm_recovery/predictions/full.json")
    ap.add_argument("--ground-truth", default="tests/algorithm_recovery/ground_truth/corpus.json")
    ap.add_argument("--show", type=int, default=15, help="samples to print per category")
    args = ap.parse_args()

    results = json.loads(Path(args.results).read_text(encoding="utf-8"))
    pred_doc = json.loads(Path(args.predictions).read_text(encoding="utf-8"))
    pred = pred_doc.get("predictions", pred_doc)
    pred_raw = pred_doc.get("predictions_raw", {})
    gt = json.loads(Path(args.ground_truth).read_text(encoding="utf-8"))

    summary = results.get("summary", {})
    print(f"mean_f1={summary.get('mean_f1')} mean_f1_raw={summary.get('mean_f1_raw')} "
          f"decompiled={summary.get('decompiled')}")

    for label, key in (("fallback", "per_binary"), ("raw", "per_binary_raw")):
        pb = results.get(key, {})
        if not pb:
            continue
        z, p, ok = summarize(pb)
        print(f"\n[{label}] f1=0: {z}  partial: {p}  perfect: {ok}")
        zeros = [k for k, v in pb.items() if v.get("f1", 0) == 0]
        for k in zeros[: args.show]:
            pr = pred_raw.get(k, pred.get(k, [])) if label == "raw" else pred.get(k, [])
            print(f"  zero {k} gt={gt.get(k, [])} pred={pr}")

    per_opt_raw = results.get("per_opt_raw", {})
    if per_opt_raw:
        print("\nper_opt_raw:")
        for opt, m in sorted(per_opt_raw.items()):
            print(f"  {opt}: mean_f1={m.get('mean_f1')} binaries={m.get('binaries')}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
