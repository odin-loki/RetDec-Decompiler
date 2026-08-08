#!/usr/bin/env bash
# run_all_automation.sh — Run every locally automatable MASTER-UPGRADE-PLAN check.
# Usage: bash scripts/run_all_automation.sh [--skip-migration] [--skip-support-regen]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="$("${ROOT}/scripts/find_python.sh")"
SKIP_MIGRATION=false
SKIP_SUPPORT=false

while [[ $# -gt 0 ]]; do
	case "$1" in
		--skip-migration) SKIP_MIGRATION=true; shift ;;
		--skip-support-regen) SKIP_SUPPORT=true; shift ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

echo "==> RetDec run_all_automation (python=${PYTHON})"
echo ""

echo "==> eval venv"
bash "${ROOT}/scripts/setup_eval_venv.sh"
PYTHON="$("${ROOT}/scripts/find_python.sh")"
echo "Using python: ${PYTHON}"
echo ""

echo "==> doctor"
bash "${ROOT}/scripts/doctor.sh"
echo ""

echo "==> ship_checklist"
bash "${ROOT}/scripts/ship_checklist.sh"
echo ""

echo "==> algorithm_recovery unit tests"
"${PYTHON}" "${ROOT}/tests/algorithm_recovery/test_labels.py"
"${PYTHON}" "${ROOT}/tests/algorithm_recovery/test_ground_truth.py"
"${PYTHON}" "${ROOT}/tests/algorithm_recovery/test_regression_gate.py"
"${PYTHON}" "${ROOT}/tests/algorithm_recovery/test_triton_gate.py"
echo ""

if [[ "${SKIP_SUPPORT}" != true ]]; then
	echo "==> retdec-support regen (staging)"
	bash "${ROOT}/scripts/regenerate-retdec-support.sh" || echo "WARN  support regen staging incomplete"
	echo ""
fi

DEC=""
for candidate in \
	"${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler" \
	"${ROOT}/build/linux/bin/retdec-decompiler"; do
	if [[ -x "${candidate}" ]]; then
		DEC="${candidate}"
		break
	fi
done
if [[ -z "${DEC}" ]]; then
	DEC="$(find "${ROOT}/build" -name 'retdec-decompiler' -type f 2>/dev/null | head -n1 || true)"
fi

if [[ "${SKIP_MIGRATION}" != true ]]; then
	echo "==> migration eval suite"
	if [[ -n "${DEC}" && -x "${DEC}" ]]; then
		bash "${ROOT}/scripts/migration_eval_suite.sh" --decompiler "${DEC}"
	else
		bash "${ROOT}/scripts/migration_eval_suite.sh" || true
	fi
	echo ""
fi

if [[ -n "${DEC}" && -x "${DEC}" && -d "${ROOT}/tests/algorithm_recovery/corpus" ]]; then
	echo "==> algorithm recovery CI core"
	bash "${ROOT}/scripts/run_algorithm_recovery_ci.sh" --decompiler "${DEC}" || echo "WARN  CI F1 pipeline failed"
	echo ""
fi

echo "==> nightly report"
bash "${ROOT}/scripts/nightly_report.sh" || true
echo ""

echo "run_all_automation: complete"
bash "${ROOT}/scripts/automation_status.sh"
