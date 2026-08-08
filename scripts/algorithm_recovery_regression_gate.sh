#!/usr/bin/env bash
# algorithm_recovery_regression_gate.sh — Fail if F1/decompiled drops vs baseline.
# Usage: bash scripts/algorithm_recovery_regression_gate.sh [--baseline FILE] [--current FILE] [--profile ci_core|full_corpus]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE="${ROOT}/results/baseline-algorithm-recovery.json"
CURRENT=""
PROFILE="ci_core"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--baseline) BASELINE="$2"; shift 2 ;;
		--current) CURRENT="$2"; shift 2 ;;
		--profile) PROFILE="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ -z "${CURRENT}" ]]; then
	CURRENT="${ROOT}/results/algorithm-recovery-ci.json"
fi

if [[ ! -f "${BASELINE}" ]]; then
	echo "Missing baseline: ${BASELINE}" >&2
	exit 1
fi
if [[ ! -f "${CURRENT}" ]]; then
	echo "Missing current results: ${CURRENT}" >&2
	exit 1
fi

python3 - "${BASELINE}" "${CURRENT}" "${PROFILE}" <<'PY'
import json, sys

baseline_path, current_path, profile = sys.argv[1:4]
baseline = json.load(open(baseline_path, encoding="utf-8"))
current = json.load(open(current_path, encoding="utf-8"))
base_m = baseline.get("metrics", {}).get(profile, {})
thresholds = baseline.get("thresholds", {})

decompiled = current.get("summary", {}).get("decompiled")
mean_f1 = float(current.get("summary", {}).get("mean_f1", 0.0))
base_dec = int(base_m.get("min_decompiled", 0))
base_f1 = float(base_m.get("mean_f1", 0.0))
max_f1_drop = float(thresholds.get("mean_f1_drop_max", 0.05))
max_dec_drop = int(thresholds.get("decompiled_drop_max", 10))

failures = []
if decompiled is None:
    failures.append("missing decompiled count in current results")
else:
    if decompiled < base_dec:
        failures.append(f"decompiled {decompiled} < baseline floor {base_dec}")
    elif base_dec and (base_dec - decompiled) > max_dec_drop:
        failures.append(f"decompiled dropped by {base_dec - decompiled} (max {max_dec_drop})")

f1_drop = base_f1 - mean_f1
print(f"profile={profile} decompiled={decompiled} mean_f1={mean_f1:.4f} baseline_f1={base_f1:.4f} drop={f1_drop:.4f}")
if f1_drop > max_f1_drop:
    failures.append(f"mean_f1 dropped by {f1_drop:.4f} (max {max_f1_drop:.4f})")

if failures:
    print("REGRESSION:", "; ".join(failures), file=sys.stderr)
    sys.exit(1)
print("Algorithm recovery regression gate: PASS")
PY
