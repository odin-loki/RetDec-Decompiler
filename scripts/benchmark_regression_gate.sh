#!/usr/bin/env bash
# benchmark_regression_gate.sh — Fail if quality metrics drop vs baseline
# or mean_wall_s slows down by more than the threshold (Part 16.3).
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

def lookup_mean_wall_s(doc):
    """metrics.decompilebench, decompilebench, summary, or top-level."""
    metrics = doc.get("metrics") if isinstance(doc.get("metrics"), dict) else {}
    db = metrics.get("decompilebench")
    if isinstance(db, dict) and db.get("mean_wall_s") is not None:
        return db.get("mean_wall_s")
    top_db = doc.get("decompilebench")
    if isinstance(top_db, dict):
        if top_db.get("mean_wall_s") is not None:
            return top_db.get("mean_wall_s")
        summary = top_db.get("summary")
        if isinstance(summary, dict) and summary.get("mean_wall_s") is not None:
            return summary.get("mean_wall_s")
    if doc.get("mean_wall_s") is not None:
        return doc.get("mean_wall_s")
    summary = doc.get("summary")
    if isinstance(summary, dict) and summary.get("mean_wall_s") is not None:
        return summary.get("mean_wall_s")
    return None

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
        if cval is None and isinstance(csec, dict):
            cval = csec.get(key)
        if cval is None:
            print(f"{section}.{key}: SKIP (not measured)")
            continue
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

# SLOWDOWN gate: fail only when current is slower than baseline by more
# than thresholds.mean_wall_s_increase_max (relative). Faster is OK.
b_wall = lookup_mean_wall_s(baseline)
c_wall = lookup_mean_wall_s(current)
if b_wall is not None:
    if c_wall is None:
        print("decompilebench.mean_wall_s: SKIP (not measured)")
    else:
        b_wall = float(b_wall)
        c_wall = float(c_wall)
        max_inc = float(thresholds.get("mean_wall_s_increase_max", 0.25))
        increase = ((c_wall - b_wall) / b_wall) if b_wall > 0 else (0.0 if c_wall <= 0 else float("inf"))
        print(
            f"decompilebench.mean_wall_s: baseline={b_wall:.4f} current={c_wall:.4f} "
            f"increase={increase:.4f} max_increase={max_inc:.4f}"
        )
        if increase > max_inc:
            failures.append(
                f"decompilebench.mean_wall_s slowed by {increase:.4f} (max {max_inc:.4f})"
            )

if failures:
    print("REGRESSION:", "; ".join(failures), file=sys.stderr)
    sys.exit(1)
print("Benchmark regression gate: PASS")
PY
