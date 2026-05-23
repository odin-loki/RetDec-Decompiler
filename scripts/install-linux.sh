#!/usr/bin/env bash
# install-linux.sh — User-facing wrapper to build or install RetDec on Linux.
#
# Usage:
#   ./scripts/install-linux.sh --build [build-linux-installer options...]
#   ./scripts/install-linux.sh [--prefix DIR] [--user] [--add-path] TARBALL
#   ./scripts/install-linux.sh --from-stage STAGE_DIR [install.sh options...]
#
# Examples:
#   # Build Release tree, install prefix, tarball (+ AppImage when APPIMAGE=1):
#   ./scripts/install-linux.sh --build --build
#
#   # Install a downloaded tarball to ~/.local/retdec:
#   ./scripts/install-linux.sh --user --add-path dist/retdec-5.0-linux-x64.tar.gz
#
#   # Configure + build + package in one shot from a clean tree:
#   cmake --preset full-linux-release
#   cmake --build build/linux --parallel
#   ./scripts/install-linux.sh --build
#
# Make this script executable once after checkout:
#   chmod +x scripts/install-linux.sh scripts/build-linux-installer.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(diname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/retdec-env.sh
source "${SCRIPT_DIR}/lib/retdec-env.sh"

MODE=""
TARBALL=""
STAGE_DIR=""
INSTALL_ARGS=()
BUILD_ARGS=()

usage() {
	cat <<'EOF'
Usage:
  install-linux.sh --build [options passed to build-linux-installer.sh]
  install-linux.sh [--prefix DIR] [--user] [--add-path] [--yes] TARBALL
  install-linux.sh --from-stage STAGE_DIR [install.sh options...]

Install options (tarball / --from-stage):
  --prefix DIR    Target install root
  --user          Install to $HOME/.local/retdec
  --add-path      Add bin/ to PATH in ~/.bashrc
  --yes           Non-interactive defaults

Build options are forwarded to scripts/build-linux-installer.sh (--build-dir,
--install-dir, --dist-dir, --version, --preset, --build, --skip-install, --dry-run).
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--build)
			MODE="build"
			shift
			;;
		--from-stage)
			MODE="stage"
			STAGE_DIR="$2"
			shift 2
			;;
		--prefix|--user|--add-path|--yes)
			INSTALL_ARGS+=("$1")
			[[ "$1" == "--prefix" ]] && { INSTALL_ARGS+=("$2"); shift; }
			shift
			;;
		--help|-h)
			usage
			exit 0
			;;
		-*)
			if [[ "$MODE" == "build" || -z "$MODE" ]]; then
				MODE="build"
				BUILD_ARGS+=("$1")
				[[ "$1" == --build-dir || "$1" == --install-dir || "$1" == --dist-dir || \
				   "$1" == --version || "$1" == --preset ]] && {
					BUILD_ARGS+=("$2")
					shift
				}
				shift
			else
				echo "Unknown option: $1" >&2
				usage >&2
				exit 1
			fi
			;;
		*)
			if [[ "$MODE" == "build" ]]; then
				echo "Unexpected argument for --build: $1" >&2
				exit 1
			fi
			if [[ -z "$MODE" ]]; then
				MODE="tarball"
			fi
			if [[ "$MODE" == "tarball" ]]; then
				TARBALL="$1"
			else
				INSTALL_ARGS+=("$1")
			fi
			shift
			;;
	esac
done

[[ -n "$MODE" ]] || { usage >&2; exit 1; }

case "$MODE" in
	build)
		echo "=== RetDec Linux build + package ==="
		exec "${SCRIPT_DIR}/build-linux-installer.sh" "${BUILD_ARGS[@]}"
		;;
	stage)
		[[ -d "$STAGE_DIR" ]] || { echo "ERROR: stage dir not found: $STAGE_DIR" >&2; exit 1; }
		[[ -x "${STAGE_DIR}/install.sh" ]] || chmod +x "${STAGE_DIR}/install.sh" "${STAGE_DIR}/uninstall.sh" 2>/dev/null || true
		exec "${STAGE_DIR}/install.sh" "${INSTALL_ARGS[@]}"
		;;
	tarball)
		[[ -n "$TARBALL" ]] || { echo "ERROR: tarball path required" >&2; usage >&2; exit 1; }
		[[ -f "$TARBALL" ]] || { echo "ERROR: tarball not found: $TARBALL" >&2; exit 1; }

		_work="${TMPDIR:-/tmp}/retdec-install-$$"
		mkdir -p "$_work"
		trap 'rm -rf "$_work"' EXIT

		echo "=== Extracting $(basename "$TARBALL") ==="
		tar xzf "$TARBALL" -C "$_work"
		_root=$(find "$_work" -mindepth 1 -maxdepth 1 -type d | head -1)
		[[ -n "$_root" && -f "${_root}/install.sh" ]] || {
			echo "ERROR: extracted tree missing install.sh" >&2
			exit 1
		}
		chmod +x "${_root}/install.sh" "${_root}/uninstall.sh"
		exec "${_root}/install.sh" "${INSTALL_ARGS[@]}"
		;;
esac
