#!/usr/bin/env bash
# uninstall.sh — Remove a RetDec install created by the macOS install.sh.
#
# Usage:
#   ./uninstall.sh [--prefix DIR]
#
# With no --prefix, reads <prefix>/.retdec-install-marker when this script is
# still inside the install tree, then /usr/local/retdec, then ~/.local/retdec.

set -euo pipefail

PREFIX=""
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--prefix) PREFIX="$2"; shift 2 ;;
		-h|--help) sed -n '2,9p' "$0"; exit 0 ;;
		*) echo "Unknown option: $1" >&2; exit 1 ;;
	esac
done

if [[ -z "$PREFIX" ]]; then
	if [[ -f "${SCRIPT_DIR}/.retdec-install-marker" ]]; then
		PREFIX="$(cat "${SCRIPT_DIR}/.retdec-install-marker")"
	elif [[ -f "/usr/local/retdec/.retdec-install-marker" ]]; then
		PREFIX="/usr/local/retdec"
	elif [[ -f "${HOME}/.local/retdec/.retdec-install-marker" ]]; then
		PREFIX="${HOME}/.local/retdec"
	else
		echo "ERROR: could not determine install prefix; pass --prefix" >&2
		exit 1
	fi
fi

echo "Removing RetDec from: ${PREFIX}"
if [[ ! -d "$PREFIX" ]]; then
	echo "Nothing to remove (directory missing)."
else
	if [[ -w "$PREFIX" ]]; then rm -rf "$PREFIX"; else sudo rm -rf "$PREFIX"; fi
fi

if [[ -d "/Applications/RetDec.app" ]]; then
	read -r -p "Also remove /Applications/RetDec.app? [y/N]: " _a || _a=N
	case "${_a:-N}" in y|Y|yes|Yes) sudo rm -rf "/Applications/RetDec.app" ;; esac
fi

for _rc in "${HOME}/.zshrc" "${HOME}/.bash_profile" "${HOME}/.bashrc"; do
	[[ -f "$_rc" ]] || continue
	if grep -Fq "${PREFIX}/bin" "$_rc"; then
		sed -i '' "\|${PREFIX}/bin|d" "$_rc" 2>/dev/null || \
			sed -i.bak "\|${PREFIX}/bin|d" "$_rc" 2>/dev/null || true
		echo "Removed PATH lines referencing ${PREFIX}/bin from ${_rc}"
	fi
done
echo "RetDec uninstalled."
