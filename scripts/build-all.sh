#!/usr/bin/env bash
# build-all.sh — End-to-end Linux build: configure, compile, install, package tarball.
#
# Usage:
#   ./scripts/build-all.sh [options]
#
# Options:
#   --preset PRESET     CMake preset (default: full-linux-release)
#   --version VER       Override package version for tarball names
#   --skip-fetch        Skip fetch-large-files.sh
#   --skip-configure    Reuse existing CMake cache
#   --skip-build        Only re-run installer packaging
#
# Outputs:
#   dist/retdec-<version>-linux-x64.tar.gz
#   releases/linux/                     (git-tracked scripts + tarball)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/retdec-env.sh
source "${SCRIPT_DIR}/lib/retdec-env.sh"

PRESET="${RETDEC_CMAKE_PRESET_LINUX_REL:-full-linux-release}"
VERSION=""
SKIP_FETCH=0
SKIP_CONFIGURE=0
SKIP_BUILD=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--preset)         PRESET="$2"; shift 2 ;;
		--version)        VERSION="$2"; shift 2 ;;
		--skip-fetch)     SKIP_FETCH=1; shift ;;
		--skip-configure) SKIP_CONFIGURE=1; shift ;;
		--skip-build)     SKIP_BUILD=1; shift ;;
		-h|--help)
			sed -n '2,18p' "$0"
			exit 0
			;;
		*) echo "Unknown option: $1" >&2; exit 1 ;;
	esac
done

if [[ "$SKIP_FETCH" -eq 0 ]]; then
	echo "==> Fetching large support files"
	"${SCRIPT_DIR}/fetch-large-files.sh" || \
		"${SCRIPT_DIR}/fetch-large-files.sh" --base-url "https://raw.githubusercontent.com/odin-loki/RetDec-Decompiler/main"
fi

if [[ "$SKIP_CONFIGURE" -eq 0 ]]; then
	echo "==> cmake --preset ${PRESET}"
	cmake --preset "${PRESET}"
fi

if [[ "$SKIP_BUILD" -eq 1 ]]; then
	INSTALLER_ARGS=(--skip-install)
else
	INSTALLER_ARGS=(--build)
fi
[[ -n "$VERSION" ]] && INSTALLER_ARGS+=(--version "$VERSION")

chmod +x "${SCRIPT_DIR}/build-linux-installer.sh" "${SCRIPT_DIR}/install-linux.sh"
"${SCRIPT_DIR}/build-linux-installer.sh" "${INSTALLER_ARGS[@]}"

echo ""
echo "=== build-all.sh complete ==="
echo "  tarball:  ${RETDEC_ROOT}/dist/retdec-*-linux-x64.tar.gz"
echo "  releases: ${RETDEC_ROOT}/releases/linux"
