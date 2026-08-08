#!/usr/bin/env bash
# setup_eval_venv.sh — Create .venv-eval with migration eval Python deps (PEP 668 safe).
# Usage: bash scripts/setup_eval_venv.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="${ROOT}/.venv-eval"
REQ="${ROOT}/scripts/requirements-eval.txt"
BASE_PYTHON="$("${ROOT}/scripts/find_python.sh" 2>/dev/null || true)"

if [[ -z "${BASE_PYTHON}" ]]; then
	for c in python3 python; do
		if command -v "$c" >/dev/null 2>&1; then
			BASE_PYTHON="$c"
			break
		fi
	done
fi

if [[ -z "${BASE_PYTHON}" ]]; then
	echo "No python interpreter found for eval venv" >&2
	exit 1
fi

if [[ ! -d "${VENV}" ]]; then
	echo "Creating ${VENV}"
	"${BASE_PYTHON}" -m venv "${VENV}"
fi

if [[ -x "${VENV}/bin/pip" ]]; then
	PIP="${VENV}/bin/pip"
	PY="${VENV}/bin/python"
elif [[ -x "${VENV}/Scripts/pip.exe" ]]; then
	PIP="${VENV}/Scripts/pip.exe"
	PY="${VENV}/Scripts/python.exe"
else
	echo "Eval venv missing pip" >&2
	exit 1
fi

"${PIP}" install -q -U pip
"${PIP}" install -q -r "${REQ}"
echo "Eval venv ready: ${PY}"
