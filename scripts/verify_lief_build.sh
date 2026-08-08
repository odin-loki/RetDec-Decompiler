#!/usr/bin/env bash
# verify_lief_build.sh — Optional build of fileformat with RETDEC_ENABLE_LIEF=ON.
# Usage: bash scripts/verify_lief_build.sh
# Exits 0 with SKIP when LIEF SDK is not installed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIEF_DIR_ARG=""
has_lief=false
if dpkg -s liblief-dev >/dev/null 2>&1; then
	has_lief=true
fi
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists lief 2>/dev/null; then
	has_lief=true
fi
SDK_CMAKE="${ROOT}/deps/lief-sdk/lib/cmake/LIEF/LIEFConfig.cmake"
if [[ -f "${SDK_CMAKE}" ]]; then
	has_lief=true
	LIEF_DIR_ARG="-DLIEF_DIR=${ROOT}/deps/lief-sdk/lib/cmake/LIEF"
elif [[ -n "${LIEF_DIR:-}" && -f "${LIEF_DIR}/LIEFConfig.cmake" ]]; then
	has_lief=true
	LIEF_DIR_ARG="-DLIEF_DIR=${LIEF_DIR}"
fi

if [[ "${has_lief}" != true ]]; then
	echo "SKIP LIEF verify: no LIEF C++ SDK found."
	echo "  Ubuntu 24.04+ has no liblief-dev apt package (Jammy-only, and 0.9.0)."
	echo "  Run: bash scripts/install_lief_sdk.sh"
	echo "  Then: export LIEF_DIR=\"\$(pwd)/deps/lief-sdk/lib/cmake/LIEF\""
	exit 0
fi

# Reuse main Linux build when present (much faster than isolated full tree).
BUILD="${ROOT}/build/linux"
if [[ ! -f "${BUILD}/CMakeCache.txt" ]]; then
	BUILD="${ROOT}/build/lief-verify"
fi

echo "==> LIEF verify (${BUILD})"
if [[ ! -f "${BUILD}/CMakeCache.txt" ]]; then
	rm -rf "${BUILD}"
	cmake -S "${ROOT}" -B "${BUILD}" -G Ninja \
		-DRETDEC_ENABLE_LIEF=ON \
		-DRETDEC_ENABLE_CUDA_ACCEL=OFF \
		-DRETDEC_ENABLE_NEURAL=OFF \
		-DRETDEC_TESTS=ON \
		${LIEF_DIR_ARG}
else
	cmake -S "${ROOT}" -B "${BUILD}" \
		-DRETDEC_ENABLE_LIEF=ON \
		-DRETDEC_ENABLE_CUDA_ACCEL=OFF \
		-DRETDEC_ENABLE_NEURAL=OFF \
		-DRETDEC_TESTS=ON \
		${LIEF_DIR_ARG}
fi

cmake --build "${BUILD}" --parallel --target retdec-fileformat-tests fileformat

if [[ -f "${BUILD}/lief-enabled.txt" ]] && grep -q '^1$' "${BUILD}/lief-enabled.txt"; then
	echo "LIEF verify: RETDEC_HAS_LIEF enabled"
else
	echo "LIEF verify: WARN lief-enabled.txt missing or disabled" >&2
	exit 1
fi

ctest --test-dir "${BUILD}" -R 'retdec-fileformat-tests|LiefAdapter' --output-on-failure || \
	ctest --test-dir "${BUILD}" -R retdec-fileformat-tests --output-on-failure
echo "LIEF verify: PASS"
