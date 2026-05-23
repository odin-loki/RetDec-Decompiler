#!/usr/bin/env bash
# install.sh — Install a RetDec Linux tarball to /opt/retdec or ~/.local/retdec.
#
# Usage:
#   ./install.sh [--prefix DIR] [--user] [--add-path] [--yes]
#
# Options:
#   --prefix DIR   Install root (default: interactive or /opt/retdec)
#   --user         Install to $HOME/.local/retdec (same as --prefix ~/.local/retdec)
#   --add-path     Append PATH snippet to ~/.bashrc (skipped if already present)
#   --yes          Non-interactive; use defaults (/opt/retdec, no PATH change)
#
# Requires write access to the chosen prefix (sudo for /opt/retdec).
#
# This copy lives in releases/linux/ for direct checkout; tarball builds also
# embed the same script at the package root.

set -euo pipefail

PREFIX=""
ADD_PATH=0
ASSUME_YES=0
PACKAGE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
	sed -n '2,12p' "$0"
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--prefix)   PREFIX="$2"; shift 2 ;;
		--user)     PREFIX="${HOME}/.local/retdec"; shift ;;
		--add-path) ADD_PATH=1; shift ;;
		--yes)      ASSUME_YES=1; shift ;;
		-h|--help)  usage; exit 0 ;;
		*) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
	esac
done

if [[ -z "$PREFIX" ]]; then
	if [[ "$ASSUME_YES" -eq 1 ]]; then
		PREFIX="/opt/retdec"
	else
		echo "RetDec install location:"
		echo "  1) /opt/retdec          (system-wide, requires sudo) [default]"
		echo "  2) ${HOME}/.local/retdec (user-local, no sudo)"
		read -r -p "Choice [1/2]: " _choice
		case "${_choice:-1}" in
			2|user|local) PREFIX="${HOME}/.local/retdec" ;;
			*)            PREFIX="/opt/retdec" ;;
		esac
	fi
fi

PREFIX="$(cd "$(dirname "$PREFIX")" && pwd)/$(basename "$PREFIX")"

_copy_into() {
	local _dest="$1"
	local _use_sudo=0
	if [[ ! -w "$(dirname "$_dest")" ]]; then
		_use_sudo=1
	fi
	if [[ "$_use_sudo" -eq 1 ]]; then
		sudo mkdir -p "$_dest"
		for _item in bin lib lib64 share; do
			[[ -d "${PACKAGE_ROOT}/${_item}" ]] || continue
			sudo rm -rf "${_dest}/${_item}"
			sudo cp -a "${PACKAGE_ROOT}/${_item}" "${_dest}/"
		done
		echo "$_dest" | sudo tee "${_dest}/.retdec-install-marker" >/dev/null
	else
		mkdir -p "$_dest"
		for _item in bin lib lib64 share; do
			[[ -d "${PACKAGE_ROOT}/${_item}" ]] || continue
			rm -rf "${_dest}/${_item}"
			cp -a "${PACKAGE_ROOT}/${_item}" "${_dest}/"
		done
		echo "$_dest" > "${_dest}/.retdec-install-marker"
	fi
}

echo "Installing RetDec to: ${PREFIX}"
_copy_into "$PREFIX"

if [[ "$ADD_PATH" -eq 0 && "$ASSUME_YES" -eq 0 ]]; then
	read -r -p "Add ${PREFIX}/bin to PATH in ~/.bashrc? [y/N]: " _path_choice
	case "${_path_choice:-N}" in
		y|Y|yes|Yes) ADD_PATH=1 ;;
	esac
fi

if [[ "$ADD_PATH" -eq 1 ]]; then
	_snippet="# RetDec (5.0)"
	_path_line="export PATH=\"${PREFIX}/bin:\${PATH}\""
	if [[ -f "${HOME}/.bashrc" ]] && grep -Fq "${PREFIX}/bin" "${HOME}/.bashrc"; then
		echo "PATH already references ${PREFIX}/bin in ~/.bashrc"
	else
		{
			echo ""
			echo "$_snippet"
			echo "$_path_line"
		} >> "${HOME}/.bashrc"
		echo "Appended PATH snippet to ~/.bashrc (open a new shell or: source ~/.bashrc)"
	fi
fi

echo ""
echo "RetDec installed."
echo "  prefix: ${PREFIX}"
echo "  try:    ${PREFIX}/bin/retdec-decompiler --help"
