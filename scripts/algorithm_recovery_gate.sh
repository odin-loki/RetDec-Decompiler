#!/usr/bin/env bash
# algorithm_recovery_gate.sh — Live F1 gate for algorithm-recovery CI (v1.4.0).
# Usage: bash scripts/algorithm_recovery_gate.sh --results FILE [--min-decompiled N] [--min-mean-f1 F]
set -euo pipefail

RESULTS=""
MIN_DECOMPILED=6
MIN_MEAN_F1=0.0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--results) RESULTS="$2"; shift 2 ;;
		--min-decompiled) MIN_DECOMPILED="$2"; shift 2 ;;
		--min-mean-f1) MIN_MEAN_F1="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ -z "${RESULTS}" || ! -f "${RESULTS}" ]]; then
	echo "Missing results: ${RESULTS}" >&2
	exit 1
fi

python3 - "${RESULTS}" "${MIN_DECOMPILED}" "${MIN_MEAN_F1}" <<'PY'
import json, sys

path, min_dec, min_f1 = sys.argv[1:4]
min_dec = int(min_dec)
min_f1 = float(min_f1)
data = json.load(open(path, encoding="utf-8"))

decompiled = data.get("decompiled")
if decompiled is None:
    decompiled = data.get("summary", {}).get("decompiled")
mean_f1 = data.get("summary", {}).get("mean_f1", 0.0)

failures = []
if decompiled is None:
    failures.append("missing decompiled count (run extract with metadata)")
elif decompiled < min_dec:
    failures.append(f"decompiled {decompiled} < min {min_dec}")
if mean_f1 < min_f1:
    failures.append(f"mean_f1 {mean_f1:.4f} < min {min_f1:.4f}")

print(f"algorithm_recovery: decompiled={decompiled} mean_f1={mean_f1:.4f}")
if failures:
    print("GATE FAIL:", "; ".join(failures), file=sys.stderr)
    sys.exit(1)
print("Algorithm recovery gate: PASS")
PY
