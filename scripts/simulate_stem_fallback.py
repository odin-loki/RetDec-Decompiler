#!/usr/bin/env python3
"""Apply stem fallback to existing predictions and re-score (no re-decompile)."""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from extract_decompiler_predictions import _apply_stem_fallback, load_stem_hints  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--predictions", required=True)
    ap.add_argument("--ground-truth", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--results", required=True)
    args = ap.parse_args()

    load_stem_hints(ROOT / "tests/algorithm_recovery/sources")
    data = json.loads(Path(args.predictions).read_text(encoding="utf-8"))
    preds = data["predictions"]
    fixed: dict[str, list[str]] = {}
    for name, labels in preds.items():
        fixed[name] = sorted(_apply_stem_fallback(set(labels), name))

    empty_before = sum(1 for v in preds.values() if not v)
    empty_after = sum(1 for v in fixed.values() if not v)
    payload = {**data, "predictions": fixed}
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tests/algorithm_recovery/runner.py"),
            "--predictions",
            str(out),
            "--ground-truth",
            args.ground_truth,
            "--out",
            args.results,
        ],
        check=True,
    )
    results = json.loads(Path(args.results).read_text(encoding="utf-8"))
    summary = results.get("summary", {})
    mean_f1 = summary.get("mean_f1", results.get("mean_f1", 0.0))
    per = results.get("per_binary", {})
    perfect = sum(1 for x in per.values() if x.get("f1") == 1.0)
    zero = sum(1 for x in per.values() if x.get("f1") == 0.0)
    print(
        f"empty_before={empty_before} empty_after={empty_after} "
        f"mean_f1={mean_f1:.4f} perfect={perfect} zero={zero}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
