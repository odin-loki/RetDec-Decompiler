#!/usr/bin/env python3
"""Dump all partial raw F1 cases with expected vs predicted labels."""
from __future__ import annotations

import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
d = json.loads((root / "results/algorithm-recovery-full.json").read_text(encoding="utf-8"))
gt = json.loads((root / "tests/algorithm_recovery/ground_truth/corpus.json").read_text(encoding="utf-8"))
pred_raw = json.loads((root / "tests/algorithm_recovery/predictions/full.json").read_text(encoding="utf-8"))[
    "predictions_raw"
]

pb = d.get("per_binary_raw", {})
partial = [(v.get("f1", 0), k) for k, v in pb.items() if 0 < v.get("f1", 0) < 1]
partial.sort()
for f1, name in partial:
    exp = set(gt.get(name, []))
    got = set(pred_raw.get(name, []))
    print(f"{f1:.3f} {name}")
    print(f"  exp:   {sorted(exp)}")
    print(f"  got:   {sorted(got)}")
    print(f"  miss:  {sorted(exp - got)}")
    print(f"  extra: {sorted(got - exp)}")
