#!/usr/bin/env bash
# Phase 1: native Linux build + tests under <repo>/build/linux/ (preset full-linux-debug)
# Phase 2: MinGW cross -> build/linux/mingw-w64-release, install/linux/mingw-w64-release, stage -> dist/windows/
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/retdec-env.sh
source "${SCRIPT_DIR}/lib/retdec-env.sh"

TOOLCHAIN="${RETDEC_ROOT}/cmake/toolchains/windows-mingw-w64.cmake"
WIN_DEPLOY="${RETDEC_ROOT}/dist/windows"
JOBS=$(nproc)

log() { echo -e "\n\033[1;34m[$(date +%H:%M:%S)] $*\033[0m"; }
pass() { echo -e "\033[1;32m[PASS] $*\033[0m"; }
fail() { echo -e "\033[1;31m[FAIL] $*\033[0m" >&2; exit 1; }

# ── Phase 1 ────────────────────────────────────────────────────────────────
log "Phase 1 — Configure preset ${RETDEC_CMAKE_PRESET_LINUX_DEBUG}"
mkdir -p "${RETDEC_BUILD_DEBUG}"
cd "${RETDEC_ROOT}"
cmake --preset "${RETDEC_CMAKE_PRESET_LINUX_DEBUG}" \
	2>&1 | tee "${RETDEC_BUILD_DEBUG}/cmake-configure.log" \
	|| fail "cmake configure failed; see ${RETDEC_BUILD_DEBUG}/cmake-configure.log"

log "Phase 1 — Build ($JOBS cores)"
cmake --build "${RETDEC_BUILD_DEBUG}" -j"${JOBS}" \
	2>&1 | tee "${RETDEC_BUILD_DEBUG}/cmake-build.log" \
	|| fail "cmake build failed"

log "Phase 1 — Tests"
cd "${RETDEC_BUILD_DEBUG}"
ctest --output-on-failure -j"${JOBS}" \
	2>&1 | tee "${RETDEC_BUILD_DEBUG}/ctest.log" \
	|| { cat "${RETDEC_BUILD_DEBUG}/ctest.log"; fail "Tests failed"; }
cd "${RETDEC_ROOT}"
pass "Phase 1 complete"

# ── Phase 2 ────────────────────────────────────────────────────────────────
log "Phase 2 — MinGW cross (build ${RETDEC_BUILD_MINGW})"
mkdir -p "${RETDEC_BUILD_MINGW}" "${RETDEC_INSTALL_MINGW}"

cmake \
	-S "${RETDEC_ROOT}" \
	-B "${RETDEC_BUILD_MINGW}" \
	-G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="${RETDEC_INSTALL_MINGW}" \
	-DRETDEC_TESTS=OFF \
	-DRETDEC_ENABLE_RETDEC=ON \
	-DRETDEC_ENABLE_RETDEC_DECOMPILER=ON \
	-DRETDEC_ENABLE_FILEFORMAT=ON \
	-DRETDEC_ENABLE_BIN2LLVMIR=ON \
	-DRETDEC_ENABLE_LLVMIR2HLL=ON \
	-DRETDEC_ENABLE_DEMANGLER=ON \
	-DRETDEC_ENABLE_COMMON=ON \
	-DRETDEC_ENABLE_CONFIG=ON \
	2>&1 | tee "${RETDEC_BUILD_MINGW}/cmake-configure.log" \
	|| fail "Windows cmake configure failed"

log "Phase 2 — Build ($JOBS cores)"
cmake --build "${RETDEC_BUILD_MINGW}" -j"${JOBS}" \
	2>&1 | tee "${RETDEC_BUILD_MINGW}/cmake-build.log" \
	|| fail "Windows cmake build failed"

log "Phase 2 — Install to ${RETDEC_INSTALL_MINGW}"
cmake --install "${RETDEC_BUILD_MINGW}" \
	2>&1 | tee "${RETDEC_BUILD_MINGW}/cmake-install.log"

log "Stage PE binaries -> ${WIN_DEPLOY}"
rm -rf "${WIN_DEPLOY}"
mkdir -p "${WIN_DEPLOY}"

find "${RETDEC_INSTALL_MINGW}" -name "*.exe" -print0 | while IFS= read -r -d '' f; do cp -v "$f" "${WIN_DEPLOY}/"; done
find "${RETDEC_INSTALL_MINGW}" -name "*.dll" -print0 | while IFS= read -r -d '' f; do cp -v "$f" "${WIN_DEPLOY}/"; done

for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
	found=$(find /usr/x86_64-w64-mingw32 /usr/lib/gcc/x86_64-w64-mingw32 \
		-name "$dll" 2>/dev/null | head -1 || true)
	if [[ -n "${found}" && -f "${found}" ]]; then
		cp -v "${found}" "${WIN_DEPLOY}/"
	else
		echo "WARNING: could not find ${dll}"
	fi
done

pass "Phase 2 complete — Windows binaries in ${WIN_DEPLOY}"
echo "  PS> .\\dist\\windows\\retdec-decompiler.exe --help"
