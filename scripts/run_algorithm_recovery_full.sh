#!/usr/bin/env bash
# run_algorithm_recovery_full.sh — Full corpus F1 (216+ binaries, nightly).
# Usage: bash scripts/run_algorithm_recovery_full.sh [--decompiler PATH] [--jobs N]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEC=""
JOBS=4
MIN_DECOMPILED=180
# Official gate stays 0.95 (stem-era). Honest name-blind full mean is 0.056.
# Do not lower this constant. See results/algorithm-recovery-gate-finding.md
MIN_MEAN_F1=0.95

while [[ $# -gt 0 ]]; do
	case "$1" in
		--decompiler) DEC="$2"; shift 2 ;;
		--jobs) JOBS="$2"; shift 2 ;;
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

PRED="${ROOT}/tests/algorithm_recovery/predictions/full.json"
GT="${ROOT}/tests/algorithm_recovery/ground_truth/corpus.json"
RESULTS="${ROOT}/results/algorithm-recovery-full.json"
WORK="${ROOT}/build/prediction-work-full"
mkdir -p "${ROOT}/results" "${ROOT}/tests/algorithm_recovery/predictions"
rm -rf "${WORK}"

python3 "${ROOT}/scripts/extract_decompiler_predictions.py" \
	--decompiler "${DEC}" \
	--corpus "${ROOT}/tests/algorithm_recovery/corpus" \
	--manifest "${ROOT}/tests/algorithm_recovery/corpus/manifest.json" \
	--jobs "${JOBS}" \
	--work "${WORK}" \
	--out "${PRED}"

python3 "${ROOT}/tests/algorithm_recovery/runner.py" \
	--predictions "${PRED}" \
	--ground-truth "${GT}" \
	--out "${RESULTS}"

bash "${ROOT}/scripts/algorithm_recovery_gate.sh" \
	--results "${RESULTS}" \
	--min-decompiled "${MIN_DECOMPILED}" \
	--min-mean-f1 "${MIN_MEAN_F1}" \
	--min-mean-f1-raw 0.85

if [[ -f "${ROOT}/results/baseline-algorithm-recovery.json" ]]; then
	bash "${ROOT}/scripts/algorithm_recovery_regression_gate.sh" \
		--current "${RESULTS}" \
		--profile full_corpus
fi

echo "Full algorithm recovery complete: ${RESULTS}"
