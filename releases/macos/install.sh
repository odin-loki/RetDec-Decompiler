#!/usr/bin/env bash
# install.sh — Install a RetDec macOS package.
#
# Usage:
#   ./install.sh [--prefix DIR] [--user] [--add-path] [--apps] [--yes]
#
# Options:
#   --prefix DIR   Install root (default: /usr/local/retdec)
#   --user         Install to $HOME/.local/retdec
#   --add-path     Append a PATH line to ~/.zshrc (macOS default shell)
#   --apps         Also copy RetDec.app into /Applications
#   --yes          Non-interactive; take the defaults
#
# Gatekeeper: anything downloaded with a browser carries com.apple.quarantine,
# and a quarantined ad-hoc signed binary refuses to open with "cannot be opened
# because the developer cannot be verified". This script removes the attribute
# from what it installs. That is the same decision as right-clicking and
# choosing Open, made once, in the open.

set -euo pipefail

PREFIX=""
ADD_PATH=0
ASSUME_YES=0
WANT_APPS=0
PACKAGE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() { sed -n '2,16p' "$0"; }

while [[ $# -gt 0 ]]; do
	case "$1" in
		--prefix)   PREFIX="$2"; shift 2 ;;
		--user)     PREFIX="${HOME}/.local/retdec"; shift ;;
		--add-path) ADD_PATH=1; shift ;;
		--apps)     WANT_APPS=1; shift ;;
		--yes)      ASSUME_YES=1; shift ;;
		-h|--help)  usage; exit 0 ;;
		*) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
	esac
done

if [[ -z "$PREFIX" ]]; then
	if [[ "$ASSUME_YES" -eq 1 ]]; then
		PREFIX="/usr/local/retdec"
	else
		echo "RetDec install location:"
		echo "  1) /usr/local/retdec     (system-wide, may need sudo) [default]"
		echo "  2) ${HOME}/.local/retdec (user-local, no sudo)"
		read -r -p "Choice [1/2]: " _choice
		case "${_choice:-1}" in
			2|user|local) PREFIX="${HOME}/.local/retdec" ;;
			*)            PREFIX="/usr/local/retdec" ;;
		esac
	fi
fi

mkdir -p "$(dirname "$PREFIX")" 2>/dev/null || true
PREFIX="$(cd "$(dirname "$PREFIX")" && pwd)/$(basename "$PREFIX")"

_sudo=""
if [[ ! -w "$(dirname "$PREFIX")" ]]; then _sudo="sudo"; fi

echo "Installing RetDec to: ${PREFIX}"
$_sudo mkdir -p "$PREFIX"
for _item in bin lib share; do
	[[ -d "${PACKAGE_ROOT}/${_item}" ]] || continue
	$_sudo rm -rf "${PREFIX:?}/${_item}"
	$_sudo cp -a "${PACKAGE_ROOT}/${_item}" "${PREFIX}/"
done
if [[ -d "${PACKAGE_ROOT}/RetDec.app" ]]; then
	$_sudo rm -rf "${PREFIX}/RetDec.app"
	$_sudo cp -a "${PACKAGE_ROOT}/RetDec.app" "${PREFIX}/"
fi
echo "$PREFIX" | $_sudo tee "${PREFIX}/.retdec-install-marker" >/dev/null

_gguf_dest="${PREFIX}/share/retdec/models"
_gguf_name="Qwen3.5-9B-Q4_K_M.gguf"
_gguf_sha="03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8"
$_sudo mkdir -p "${_gguf_dest}"
if [[ ! -f "${_gguf_dest}/${_gguf_name}" ]]; then
	_gguf_from=""
	for _d in "${PACKAGE_ROOT}" "${PACKAGE_ROOT}/.." "${PWD}"; do
		shopt -s nullglob
		_parts=( "${_d}"/Qwen3.5-9B-Q4_K_M.gguf.part* )
		shopt -u nullglob
		if [[ ${#_parts[@]} -gt 0 ]]; then
			_gguf_from="${_d}"
			break
		fi
	done
	if [[ -n "${_gguf_from}" ]]; then
		echo "Joining GGUF from ${_gguf_from} into ${_gguf_dest}"
		_tmp="$(mktemp)"
		cat "${_gguf_from}"/Qwen3.5-9B-Q4_K_M.gguf.part* > "${_tmp}"
		if command -v sha256sum >/dev/null 2>&1; then
			_got="$(sha256sum "${_tmp}" | awk '{print $1}')"
		else
			_got="$(shasum -a 256 "${_tmp}" | awk '{print $1}')"
		fi
		if [[ "${_got}" != "${_gguf_sha}" ]]; then
			echo "GGUF SHA-256 mismatch (got ${_got})" >&2
			rm -f "${_tmp}"
			exit 1
		fi
		$_sudo mv "${_tmp}" "${_gguf_dest}/${_gguf_name}"
		echo "Installed ${_gguf_dest}/${_gguf_name}"
	else
		echo "Neural GGUF parts not found next to the package."
		echo "  Download Qwen3.5-9B-Q4_K_M.gguf.partaa (and siblings) from the GitHub Release"
		echo "  into this folder and re-run, or run scripts/join_qwen_gguf.sh"
	fi
fi

# Gatekeeper. Failure here is not fatal: a package that was never quarantined
# has no attribute to remove and xattr says so.
if command -v xattr >/dev/null 2>&1; then
	$_sudo xattr -dr com.apple.quarantine "$PREFIX" 2>/dev/null || true
fi

if [[ "$WANT_APPS" -eq 0 && "$ASSUME_YES" -eq 0 && -d "${PACKAGE_ROOT}/RetDec.app" ]]; then
	read -r -p "Copy RetDec.app into /Applications? [y/N]: " _a
	case "${_a:-N}" in y|Y|yes|Yes) WANT_APPS=1 ;; esac
fi
if [[ "$WANT_APPS" -eq 1 && -d "${PACKAGE_ROOT}/RetDec.app" ]]; then
	sudo rm -rf "/Applications/RetDec.app"
	sudo cp -a "${PACKAGE_ROOT}/RetDec.app" "/Applications/"
	sudo xattr -dr com.apple.quarantine "/Applications/RetDec.app" 2>/dev/null || true
	echo "Installed /Applications/RetDec.app"
fi

if [[ "$ADD_PATH" -eq 0 && "$ASSUME_YES" -eq 0 ]]; then
	read -r -p "Add ${PREFIX}/bin to PATH in ~/.zshrc? [y/N]: " _p
	case "${_p:-N}" in y|Y|yes|Yes) ADD_PATH=1 ;; esac
fi

if [[ "$ADD_PATH" -eq 1 ]]; then
	_rc="${HOME}/.zshrc"
	[[ "${SHELL##*/}" == "bash" ]] && _rc="${HOME}/.bash_profile"
	if [[ -f "$_rc" ]] && grep -Fq "${PREFIX}/bin" "$_rc"; then
		echo "PATH already references ${PREFIX}/bin in ${_rc}"
	else
		{
			echo ""
			echo "# RetDec (2.0.21)"
			echo "export PATH=\"${PREFIX}/bin:\${PATH}\""
		} >> "$_rc"
		echo "Appended a PATH line to ${_rc} (open a new shell, or: source ${_rc})"
	fi
fi

echo ""
echo "RetDec installed."
echo "  prefix: ${PREFIX}"
echo "  try:    ${PREFIX}/bin/retdec-decompiler --help"
