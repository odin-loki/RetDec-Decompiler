#!/usr/bin/env python3
"""Algorithm recovery regression gate logic (callable from shell or tests)."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def check_regression(baseline: dict, current: dict, profile: str = "ci_core") -> tuple[bool, str]:
    base_m = baseline.get("metrics", {}).get(profile, {})
    thresholds = baseline.get("thresholds", {})

    decompiled = current.get("summary", {}).get("decompiled")
    mean_f1 = float(current.get("summary", {}).get("mean_f1", 0.0))
    base_dec = int(base_m.get("min_decompiled", 0))
    base_f1 = float(base_m.get("mean_f1", 0.0))
    max_f1_drop = float(thresholds.get("mean_f1_drop_max", 0.05))
    max_dec_drop = int(thresholds.get("decompiled_drop_max", 10))

    failures: list[str] = []
    if decompiled is None:
        failures.append("missing decompiled count in current results")
    else:
        if decompiled < base_dec:
            failures.append(f"decompiled {decompiled} < baseline floor {base_dec}")
        elif base_dec and (base_dec - decompiled) > max_dec_drop:
            failures.append(f"decompiled dropped by {base_dec - decompiled} (max {max_dec_drop})")

    f1_drop = base_f1 - mean_f1
    msg = (
        f"profile={profile} decompiled={decompiled} mean_f1={mean_f1:.4f} "
        f"baseline_f1={base_f1:.4f} drop={f1_drop:.4f}"
    )
    if f1_drop > max_f1_drop:
        failures.append(f"mean_f1 dropped by {f1_drop:.4f} (max {max_f1_drop:.4f})")

    if failures:
        return False, f"{msg}\nREGRESSION: " + "; ".join(failures)
    return True, f"{msg}\nAlgorithm recovery regression gate: PASS"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--current", required=True)
    ap.add_argument("--profile", default="ci_core")
    args = ap.parse_args()

    baseline = json.loads(Path(args.baseline).read_text(encoding="utf-8"))
    current = json.loads(Path(args.current).read_text(encoding="utf-8"))
    ok, msg = check_regression(baseline, current, args.profile)
    print(msg)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
