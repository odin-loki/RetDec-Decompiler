#!/usr/bin/env bash
# Build + ctest using the in-repo preset tree (build/linux, preset full-linux-debug by default).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/retdec-env.sh
source "${SCRIPT_DIR}/lib/retdec-env.sh"

JOBS=$(nproc)
log() { echo -e "\n\033[1;34m[$(date +%H:%M:%S)] $*\033[0m"; }
pass() { echo -e "\033[1;32m[PASS] $*\033[0m"; }
fail() { echo -e "\033[1;31m[FAIL] $*\033[0m" >&2; exit 1; }

[[ -f "${RETDEC_BUILD_DEBUG}/CMakeCache.txt" ]] || fail "Not configured. Run: bash scripts/wsl_configure_nosudo.sh"

log "Building ($JOBS cores) -> ${RETDEC_BUILD_DEBUG}"
cmake --build "${RETDEC_BUILD_DEBUG}" -j"${JOBS}" \
	2>&1 | tee "${RETDEC_BUILD_DEBUG}/cmake-build.log" \
	|| { tail -50 "${RETDEC_BUILD_DEBUG}/cmake-build.log"; fail "Build failed"; }

pass "Build complete"

log "Tests (ctest -j${JOBS})"
cd "${RETDEC_BUILD_DEBUG}"
ctest --output-on-failure -j"${JOBS}" \
	2>&1 | tee "${RETDEC_BUILD_DEBUG}/ctest.log" \
	|| { cat "${RETDEC_BUILD_DEBUG}/ctest.log"; fail "Tests failed"; }

pass "All tests passed"
