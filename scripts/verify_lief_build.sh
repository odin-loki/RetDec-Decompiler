#!/usr/bin/env bash
# verify_lief_build.sh — Optional isolated build of fileformat with RETDEC_ENABLE_LIEF=ON.
# Usage: bash scripts/verify_lief_build.sh
# Exits 0 with SKIP when liblief-dev is not installed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build/lief-verify"

has_lief=false
if dpkg -s liblief-dev >/dev/null 2>&1; then
	has_lief=true
fi
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists lief 2>/dev/null; then
	has_lief=true
fi

if [[ "${has_lief}" != true ]]; then
	echo "SKIP LIEF verify: install liblief-dev (sudo apt install liblief-dev)"
	exit 0
fi

echo "==> LIEF verify build (${BUILD})"
rm -rf "${BUILD}"
cmake -S "${ROOT}" -B "${BUILD}" -G Ninja \
	-DRETDEC_ENABLE_LIEF=ON \
	-DRETDEC_ENABLE_CUDA_ACCEL=OFF \
	-DRETDEC_ENABLE_NEURAL=OFF

cmake --build "${BUILD}" --parallel --target retdec-fileformat-tests

if [[ -f "${BUILD}/lief-enabled.txt" ]] && grep -q '^1$' "${BUILD}/lief-enabled.txt"; then
	echo "LIEF verify: RETDEC_HAS_LIEF enabled"
else
	echo "LIEF verify: WARN lief-enabled.txt missing" >&2
	exit 1
fi

ctest --test-dir "${BUILD}" -R retdec-fileformat-tests --output-on-failure
echo "LIEF verify: PASS"
