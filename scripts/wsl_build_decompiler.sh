#!/usr/bin/env bash
# wsl_build_decompiler.sh — Clean stale CMake cache and build retdec-decompiler in WSL.
# Usage: bash scripts/wsl_build_decompiler.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

echo "==> Cleaning stale CMake cache (fixes retdec-master path mismatch)"
rm -rf build/linux/CMakeCache.txt build/linux/CMakeFiles build/linux/build.ninja

echo "==> Configure core-debug (CPU-only)"
cmake --preset core-debug \
	-DRETDEC_ENABLE_CUDA_ACCEL=OFF \
	-DRETDEC_ENABLE_NEURAL=OFF

JOBS="$(nproc 2>/dev/null || echo 4)"
echo "==> Build retdec-decompiler (-j${JOBS}) — expect 30–90 minutes on first run"
cmake --build build/linux --target retdec-decompiler --parallel "${JOBS}"

DEC="$(find build/linux -name retdec-decompiler -type f | head -n1)"
if [[ -z "${DEC}" ]]; then
	echo "retdec-decompiler not found after build" >&2
	exit 1
fi
echo "Built: ${DEC}"
echo ""
echo "Next: bash scripts/run_algorithm_recovery_ci.sh --decompiler ${DEC}"
