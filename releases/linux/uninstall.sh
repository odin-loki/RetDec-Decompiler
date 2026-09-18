#!/usr/bin/env bash
# uninstall.sh — Remove a RetDec install created by install.sh.
#
# Usage:
#   ./uninstall.sh [--prefix DIR]
#
# If --prefix is omitted, reads <prefix>/.retdec-install-marker when this script
# still lives inside the install tree, otherwise /opt/retdec then ~/.local/retdec.

set -euo pipefail

PREFIX=""
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--prefix) PREFIX="$2"; shift 2 ;;
		-h|--help)
			sed -n '2,8p' "$0"
			exit 0
			;;
		*) echo "Unknown option: $1" >&2; exit 1 ;;
	esac
done

if [[ -z "$PREFIX" ]]; then
	if [[ -f "${SCRIPT_DIR}/.retdec-install-marker" ]]; then
		PREFIX="$(cat "${SCRIPT_DIR}/.retdec-install-marker")"
	elif [[ -f "/opt/retdec/.retdec-install-marker" ]]; then
		PREFIX="/opt/retdec"
	elif [[ -f "${HOME}/.local/retdec/.retdec-install-marker" ]]; then
		PREFIX="${HOME}/.local/retdec"
	else
		echo "ERROR: could not determine install prefix; pass --prefix" >&2
		exit 1
	fi
fi

_remove_path_snippet() {
	[[ -f "${HOME}/.bashrc" ]] || return 0
	if grep -Fq "${PREFIX}/bin" "${HOME}/.bashrc"; then
		# shellcheck disable=SC2016
		sed -i.bak "\|${PREFIX}/bin|d" "${HOME}/.bashrc" 2>/dev/null || \
			sed -i '' "\|${PREFIX}/bin|d" "${HOME}/.bashrc" 2>/dev/null || true
		echo "Removed PATH lines referencing ${PREFIX}/bin from ~/.bashrc (backup: ~/.bashrc.bak)"
	fi
}

echo "Removing RetDec from: ${PREFIX}"
if [[ ! -d "$PREFIX" ]]; then
	echo "Nothing to remove (directory missing)."
	exit 0
fi

if [[ -w "$PREFIX" ]]; then
	rm -rf "$PREFIX"
else
	sudo rm -rf "$PREFIX"
fi

_remove_path_snippet
echo "RetDec uninstalled."
