#!/usr/bin/env bash
# wsl_build_decompiler.sh — Clean stale CMake cache and build retdec-decompiler in WSL.
# Usage: bash scripts/wsl_build_decompiler.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

clean_stale_cmake_caches() {
	local root_abs
	root_abs="$(cd "${ROOT}" && pwd)"
	while IFS= read -r -d '' cache; do
		if grep -q 'retdec-master' "${cache}" 2>/dev/null \
			|| ! grep -qF "${root_abs}" "${cache}" 2>/dev/null; then
			local dir
			dir="$(dirname "${cache}")"
			echo "==> Removing stale CMake cache: ${dir}"
			rm -rf "${dir}"
		fi
	done < <(find build/linux -name CMakeCache.txt -print0 2>/dev/null || true)
	find build/linux/external -path '*-stamp/*-configure' -type f -delete 2>/dev/null || true
}

echo "==> Cleaning stale CMake cache (fixes retdec-master path mismatch)"
rm -rf build/linux/CMakeCache.txt build/linux/CMakeFiles build/linux/build.ninja
clean_stale_cmake_caches

echo "==> Configure core-debug (CPU-only, neural mock)"
cmake --preset core-debug \
	-DRETDEC_ENABLE_CUDA_ACCEL=OFF \
	-DRETDEC_ENABLE_NEURAL=ON \
	-DRETDEC_ENABLE_LLAMACPP=OFF

JOBS="$(nproc 2>/dev/null || echo 4)"
echo "==> Build retdec-decompiler (-j${JOBS}) — expect 30-90 minutes on first run"
cmake --build build/linux --target retdec-decompiler --parallel "${JOBS}"
cmake --install build/linux >/dev/null 2>&1 || true

DEC="$(find build/linux -name retdec-decompiler -type f | head -n1)"
if [[ -z "${DEC}" ]]; then
	echo "retdec-decompiler not found after build" >&2
	exit 1
fi
echo "Built: ${DEC}"
echo ""
stage_decompiler_runtime_data() {
	local dec_dir
	dec_dir="$(dirname "${DEC}")"
	local share_dir="${dec_dir}/../share/retdec"
	mkdir -p "${share_dir}/profiles"
	cp "${ROOT}/src/retdec-decompiler/decompiler-config.json" "${share_dir}/"
	cp "${ROOT}/src/retdec-decompiler/profiles/"*.json "${share_dir}/profiles/"
	if [[ -d "${ROOT}/install/linux/share/retdec/support" ]]; then
		rm -rf "${share_dir}/support"
		cp -a "${ROOT}/install/linux/share/retdec/support" "${share_dir}/support"
		echo "Staged support package from install/linux"
	else
		echo "WARN: install/linux/share/retdec/support missing — run: cmake --install build/linux" >&2
	fi
	echo "Staged decompiler config: ${share_dir}/decompiler-config.json"
}
stage_decompiler_runtime_data
echo ""
echo "Next: bash scripts/run_algorithm_recovery_ci.sh --decompiler ${DEC}"
