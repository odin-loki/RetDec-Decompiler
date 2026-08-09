#!/usr/bin/env python3
"""Re-apply label refinement to cached predictions_raw and re-score."""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from simulate_raw_refine import refine_exported_labels  # noqa: E402


def main() -> int:
    pred_path = ROOT / "tests/algorithm_recovery/predictions/full.json"
    gt_path = ROOT / "tests/algorithm_recovery/ground_truth/corpus.json"
    results_path = ROOT / "results/algorithm-recovery-full.json"

    pred_doc = json.loads(pred_path.read_text(encoding="utf-8"))
    raw = pred_doc.get("predictions_raw", {})

    for name, labels in list(raw.items()):
        raw[name] = refine_exported_labels(labels, name)

    pred_doc["predictions_raw"] = raw
    pred_path.write_text(json.dumps(pred_doc, indent=2) + "\n", encoding="utf-8")

    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tests/algorithm_recovery/runner.py"),
            "--predictions",
            str(pred_path),
            "--ground-truth",
            str(gt_path),
            "--out",
            str(results_path),
        ],
        check=True,
    )
    summary = json.loads(results_path.read_text(encoding="utf-8")).get("summary", {})
    print(f"mean_f1_raw={summary.get('mean_f1_raw')} mean_f1={summary.get('mean_f1')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
