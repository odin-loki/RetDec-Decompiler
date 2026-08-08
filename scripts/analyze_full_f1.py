#!/usr/bin/env python3
import json
from pathlib import Path

r = json.loads(Path("results/algorithm-recovery-full.json").read_text())
pred = json.loads(Path("tests/algorithm_recovery/predictions/full.json").read_text())["predictions"]
pb = r["per_binary"]
zeros = [k for k, v in pb.items() if v["f1"] == 0]
partial = [k for k, v in pb.items() if 0 < v["f1"] < 1]
perfect = [k for k, v in pb.items() if v["f1"] == 1]
print(f"f1=0: {len(zeros)} partial: {len(partial)} perfect: {len(perfect)}")
print("sample zero:", zeros[:15])
for k in sorted(partial, key=lambda x: pb[x]["f1"])[:10]:
    print(f"  partial {pb[k]['f1']:.2f} {k} pred={pred.get(k,[])}")
gt = json.loads(Path("tests/algorithm_recovery/ground_truth/corpus.json").read_text())
for k in zeros[:10]:
    print(f"  zero {k} gt={gt.get(k,[])} pred={pred.get(k,[])}")
