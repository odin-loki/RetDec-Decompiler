#!/usr/bin/env bash
# Configure RetDec in WSL using the same layout as Windows: <repo>/build/<preset>/
# Default preset (full-linux-debug) requires Qt6 — e.g. sudo apt install qt6-base-dev qt6-base-dev-tools
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/retdec-env.sh
source "${SCRIPT_DIR}/lib/retdec-env.sh"

cd "$RETDEC_ROOT"
mkdir -p "$RETDEC_BUILD_DEBUG"
echo "[$(date +%H:%M:%S)] cmake --preset ${RETDEC_CMAKE_PRESET_LINUX_DEBUG} (build -> ${RETDEC_BUILD_DEBUG})"
cmake --preset "${RETDEC_CMAKE_PRESET_LINUX_DEBUG}" 2>&1 | tee "${RETDEC_BUILD_DEBUG}/cmake-configure.log"
exit "${PIPESTATUS[0]}"
