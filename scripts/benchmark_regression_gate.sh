#!/usr/bin/env bash
# benchmark_regression_gate.sh — Fail if metrics drop vs baseline (Part 16.3).
# Usage: bash scripts/benchmark_regression_gate.sh [--baseline FILE] [--current FILE]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASELINE="${ROOT}/results/baseline-2026-08.json"
CURRENT=""

while [[ $# -gt 0 ]]; do
	case "$1" in
		--baseline) BASELINE="$2"; shift 2 ;;
		--current) CURRENT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ -z "${CURRENT}" ]]; then
	SHA="$(git -C "${ROOT}" rev-parse HEAD 2>/dev/null || echo local)"
	CURRENT="${ROOT}/results/${SHA}.json"
fi

if [[ ! -f "${BASELINE}" ]]; then
	echo "Missing baseline: ${BASELINE}" >&2
	exit 1
fi
if [[ ! -f "${CURRENT}" ]]; then
	echo "Missing current results: ${CURRENT}" >&2
	echo "Run: bash scripts/run_benchmarks.sh" >&2
	exit 1
fi

python3 - "${BASELINE}" "${CURRENT}" <<'PY'
import json, sys

baseline_path, current_path = sys.argv[1:3]
baseline = json.load(open(baseline_path, encoding="utf-8"))
current = json.load(open(current_path, encoding="utf-8"))
thresholds = baseline.get("thresholds", {})
base_m = baseline.get("metrics", {})
cur_m = current.get("metrics", baseline.get("metrics", {}))

def rate(deck, key):
    samples = deck.get("samples", [])
    if not samples:
        return deck.get(key)
    if key == "syntax_valid_rate":
        return sum(1 for s in samples if s.get("syntax_valid")) / len(samples)
    if key == "recompile_success_rate":
        ok = [s for s in samples if s.get("recompile_success") is True]
        denom = sum(1 for s in samples if s.get("recompile_success") is not None)
        return (len(ok) / denom) if denom else 0.0
    return deck.get(key)

failures = []
for section, keys in (
    ("decompilebench", ("syntax_valid_rate", "recompile_success_rate", "coverage_equivalence_rate")),
    ("algorithm_recovery", ("mean_f1",)),
):
    bsec = base_m.get(section, {})
    csec = cur_m.get(section, current.get(section, {}))
    for key in keys:
        bval = bsec.get(key)
        if bval is None:
            continue
        cval = rate(csec, key) if isinstance(csec, dict) else None
        if cval is None:
            cval = csec.get(key, bval)
        drop = float(bval) - float(cval)
        max_drop = float(thresholds.get(f"{key.replace('_rate', '')}_drop_max", thresholds.get("recompile_success_drop_max", 0.05)))
        if key == "syntax_valid_rate":
            max_drop = float(thresholds.get("syntax_valid_drop_max", 0.05))
        if key == "mean_f1":
            max_drop = float(thresholds.get("mean_f1_drop_max", 0.05))
        if key == "coverage_equivalence_rate":
            max_drop = float(thresholds.get("coverage_equivalence_drop_max", 0.05))
        print(f"{section}.{key}: baseline={bval:.4f} current={cval:.4f} drop={drop:.4f} max_drop={max_drop:.4f}")
        if drop > max_drop:
            failures.append(f"{section}.{key} dropped by {drop:.4f} (max {max_drop:.4f})")

if failures:
    print("REGRESSION:", "; ".join(failures), file=sys.stderr)
    sys.exit(1)
print("Benchmark regression gate: PASS")
PY
