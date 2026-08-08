#!/usr/bin/env bash
# run_algorithm_recovery_ci.sh — Live F1 on CI core subset (v1.4.0).
# Usage: bash scripts/run_algorithm_recovery_ci.sh [--decompiler PATH]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEC=""
MIN_DECOMPILED=6
MIN_MEAN_F1=0.0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--decompiler) DEC="$2"; shift 2 ;;
		--min-decompiled) MIN_DECOMPILED="$2"; shift 2 ;;
		--min-mean-f1) MIN_MEAN_F1="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ -z "${DEC}" ]]; then
	for candidate in \
		"${ROOT}/build/linux/bin/retdec-decompiler" \
		"${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler" \
		"$(command -v retdec-decompiler 2>/dev/null || true)"; do
		if [[ -n "${candidate}" && -x "${candidate}" ]]; then
			DEC="${candidate}"
			break
		fi
	done
fi

if [[ -z "${DEC}" || ! -x "${DEC}" ]]; then
	echo "retdec-decompiler not found — set --decompiler" >&2
	exit 1
fi

bash "${ROOT}/scripts/build_algorithm_corpus.sh"

PRED="${ROOT}/tests/algorithm_recovery/predictions/ci.json"
GT="${ROOT}/tests/algorithm_recovery/ground_truth/corpus.json"
RESULTS="${ROOT}/results/algorithm-recovery-ci.json"
mkdir -p "${ROOT}/results" "${ROOT}/tests/algorithm_recovery/predictions"

python3 "${ROOT}/scripts/extract_decompiler_predictions.py" \
	--decompiler "${DEC}" \
	--corpus "${ROOT}/tests/algorithm_recovery/corpus" \
	--manifest "${ROOT}/tests/algorithm_recovery/corpus/manifest.json" \
	--ci-core \
	--out "${PRED}"

python3 "${ROOT}/tests/algorithm_recovery/runner.py" \
	--predictions "${PRED}" \
	--ground-truth "${GT}" \
	--out "${RESULTS}"

bash "${ROOT}/scripts/algorithm_recovery_gate.sh" \
	--results "${RESULTS}" \
	--min-decompiled "${MIN_DECOMPILED}" \
	--min-mean-f1 "${MIN_MEAN_F1}"

if [[ -f "${ROOT}/results/baseline-algorithm-recovery.json" ]]; then
	bash "${ROOT}/scripts/algorithm_recovery_regression_gate.sh" \
		--current "${RESULTS}" \
		--profile ci_core || true
fi

echo "Algorithm recovery CI complete: ${RESULTS}"
