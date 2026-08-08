#!/usr/bin/env python3
"""Unit tests for algorithm_recovery_regression_gate logic."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from algorithm_recovery_regression_gate import check_regression  # noqa: E402


def main() -> int:
    baseline = {
        "metrics": {"ci_core": {"min_decompiled": 6, "mean_f1": 0.1}},
        "thresholds": {"mean_f1_drop_max": 0.05, "decompiled_drop_max": 10},
    }
    ok_current = {"summary": {"decompiled": 8, "mean_f1": 0.12}}
    bad_current = {"summary": {"decompiled": 8, "mean_f1": 0.0}}

    ok, _ = check_regression(baseline, ok_current)
    if not ok:
        print("expected PASS for improving metrics", file=sys.stderr)
        return 1
    bad, _ = check_regression(baseline, bad_current)
    if bad:
        print("expected FAIL for F1 drop", file=sys.stderr)
        return 1

    print("algorithm_recovery regression gate tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
