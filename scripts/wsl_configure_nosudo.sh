#!/usr/bin/env bash
# Configure RetDec in WSL using the same layout as Windows: <repo>/build/<preset>/
#
# Usage (works when not executable):
#   bash scripts/wsl_configure_nosudo.sh
#   ./scripts/wsl_configure_nosudo.sh
#
# Options:
#   (none) — uses RETDEC_CMAKE_PRESET_LINUX_DEBUG from scripts/lib/retdec-env.sh
#
# Requires:
#   cmake 3.26+, Ninja or Make generator from the preset
#   Qt6 for full-linux-debug (e.g. sudo apt install qt6-base-dev qt6-base-dev-tools)
#
# Help:
#   bash scripts/wsl_configure_nosudo.sh --help
#   grep '^#' "$0" | sed -E 's/^#\s?//'
set -euo pipefail

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    grep -E '^#' "$0" | sed -E 's/^#\s?//'
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/retdec-env.sh
source "${SCRIPT_DIR}/lib/retdec-env.sh"

cd "$RETDEC_ROOT"
mkdir -p "$RETDEC_BUILD_DEBUG"
echo "[$(date +%H:%M:%S)] cmake --preset ${RETDEC_CMAKE_PRESET_LINUX_DEBUG} (build -> ${RETDEC_BUILD_DEBUG})"
cmake --preset "${RETDEC_CMAKE_PRESET_LINUX_DEBUG}" 2>&1 | tee "${RETDEC_BUILD_DEBUG}/cmake-configure.log"
exit "${PIPESTATUS[0]}"
