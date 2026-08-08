#!/usr/bin/env bash
# ship_checklist.sh — Pre-release validation (Part 12.4).
# Usage: bash scripts/ship_checklist.sh [--version X.Y.Z]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPECTED=""
FAIL=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--version) EXPECTED="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

check() {
	if "$@"; then
		echo "PASS  $*"
	else
		echo "FAIL  $*" >&2
		FAIL=$((FAIL + 1))
	fi
}

VER_CMAKE="$(grep -E '^[[:space:]]*VERSION[[:space:]]' "${ROOT}/CMakeLists.txt" | head -1 | sed -E 's/.*VERSION[[:space:]]+([0-9.]+).*/\1/')"
VER_RELEASE="$(grep -E '^version=' "${ROOT}/releases/VERSION" | cut -d= -f2)"

echo "==> RetDec ship checklist"
echo "CMake VERSION: ${VER_CMAKE}"
echo "releases/VERSION: ${VER_RELEASE}"

if [[ -n "${EXPECTED}" ]]; then
	check test "${VER_CMAKE}" = "${EXPECTED}"
	check test "${VER_RELEASE}" = "${EXPECTED}"
else
	check test "${VER_CMAKE}" = "${VER_RELEASE}"
fi

check test -f "${ROOT}/LICENSE"
check test -f "${ROOT}/LICENSE-AGPL"
check test -f "${ROOT}/LICENSE-COMMERCIAL"
check test -f "${ROOT}/NOTICE"
check test -f "${ROOT}/results/baseline-2026-08.json"
check test -f "${ROOT}/results/baseline-algorithm-recovery.json"
check test -f "${ROOT}/docs/internal/PLAN_COMPLETION.md"
check bash "${ROOT}/scripts/doctor.sh"
check python3 "${ROOT}/tests/algorithm_recovery/test_labels.py"
check python3 "${ROOT}/tests/algorithm_recovery/test_regression_gate.py"
check python3 "${ROOT}/tests/algorithm_recovery/test_triton_gate.py"

if grep -qE '5\.0-rc|v4\.2\.0-rc' "${ROOT}/cmake/deps.cmake" 2>/dev/null; then
	echo "FAIL  RC dependency pins in deps.cmake" >&2
	FAIL=$((FAIL + 1))
else
	echo "PASS  no RC dependency pins"
fi

if grep -q 'specification-extraction' "${ROOT}/README.md"; then
	echo "PASS  D7 positioning in README"
else
	echo "FAIL  D7 positioning missing from README" >&2
	FAIL=$((FAIL + 1))
fi

echo ""
if [[ "${FAIL}" -gt 0 ]]; then
	echo "Ship checklist: ${FAIL} failure(s)" >&2
	exit 1
fi
echo "Ship checklist: ALL PASS"
