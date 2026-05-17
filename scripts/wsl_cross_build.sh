#!/usr/bin/env bash
# Build MinGW cross tree and stage PE binaries to dist/windows/ (same layout as wsl_build.sh phase 2).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/retdec-env.sh
source "${SCRIPT_DIR}/lib/retdec-env.sh"

WIN_DEPLOY="${RETDEC_ROOT}/dist/windows"
JOBS=$(nproc)

log() { echo -e "\n\033[1;34m[$(date +%H:%M:%S)] $*\033[0m"; }
fail() { echo -e "\033[1;31m[FAIL] $*\033[0m" >&2; exit 1; }

[[ -f "${RETDEC_BUILD_MINGW}/CMakeCache.txt" ]] || fail "Not configured. Run: bash scripts/wsl_cross_configure.sh"

log "Building (${JOBS} cores) -> ${RETDEC_BUILD_MINGW}"
cmake --build "${RETDEC_BUILD_MINGW}" -j"${JOBS}" \
	2>&1 | tee "${RETDEC_BUILD_MINGW}/cmake-build.log" \
	|| fail "Build failed — see ${RETDEC_BUILD_MINGW}/cmake-build.log"

log "Install -> ${RETDEC_INSTALL_MINGW}"
cmake --install "${RETDEC_BUILD_MINGW}" 2>&1 | tee "${RETDEC_BUILD_MINGW}/cmake-install.log"

log "Stage -> ${WIN_DEPLOY}"
rm -rf "${WIN_DEPLOY}"
mkdir -p "${WIN_DEPLOY}"
find "${RETDEC_INSTALL_MINGW}" -name "*.exe" -print0 | while IFS= read -r -d '' f; do cp -v "$f" "${WIN_DEPLOY}/"; done
find "${RETDEC_INSTALL_MINGW}" -name "*.dll" -print0 | while IFS= read -r -d '' f; do cp -v "$f" "${WIN_DEPLOY}/"; done

for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
	found=$(find /usr/x86_64-w64-mingw32 /usr/lib/gcc/x86_64-w64-mingw32 \
		-name "$dll" 2>/dev/null | head -1 || true)
	if [[ -n "${found}" && -f "${found}" ]]; then
		cp -v "${found}" "${WIN_DEPLOY}/"
	fi
done

echo "Done: ${WIN_DEPLOY}"
