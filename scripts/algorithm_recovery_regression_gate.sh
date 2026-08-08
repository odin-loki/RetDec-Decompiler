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

exec python3 "${ROOT}/scripts/algorithm_recovery_regression_gate.py" \
	--baseline "${BASELINE}" \
	--current "${CURRENT}" \
	--profile "${PROFILE}"
