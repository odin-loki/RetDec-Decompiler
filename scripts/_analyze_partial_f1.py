#!/usr/bin/env python3
"""Summarize raw partial F1 gaps from algorithm-recovery-full.json."""
from __future__ import annotations

import json
from collections import Counter
from pathlib import Path

root = Path(__file__).resolve().parents[1]
d = json.loads((root / "results/algorithm-recovery-full.json").read_text(encoding="utf-8"))
gt = json.loads((root / "tests/algorithm_recovery/ground_truth/corpus.json").read_text(encoding="utf-8"))
pred_doc = json.loads((root / "tests/algorithm_recovery/predictions/full.json").read_text(encoding="utf-8"))
pred_raw = pred_doc.get("predictions_raw", {})

pb = d.get("per_binary_raw", {})
partial = [(v.get("f1", 0), k) for k, v in pb.items() if 0 < v.get("f1", 0) < 1]
partial.sort()
print(f"partial count: {len(partial)}")
stems: Counter[str] = Counter()
for _, name in partial:
    stem = name.lower()
    for sfx in ("-gcc-o0", "-gcc-o2", "-gcc-o3", "-clang-o0", "-clang-o2", "-clang-o3"):
        if stem.endswith(sfx):
            stem = stem[: -len(sfx)]
            break
    if stem.startswith("generated_"):
        stem = stem[len("generated_") :]
    stems[stem] += 1
print("\nby stem:")
for stem, n in stems.most_common():
    print(f"  {n:3d} {stem}")

print("\nsamples:")
for f1, name in partial[:20]:
    exp = set(gt.get(name, []))
    got = set(pred_raw.get(name, []))
    missing = sorted(exp - got)
    extra = sorted(got - exp)
    print(f"{f1:.3f} {name}")
    if missing:
        print(f"  missing: {missing}")
    if extra:
        print(f"  extra:   {extra}")
