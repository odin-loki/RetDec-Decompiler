#!/usr/bin/env bash
# build-macos-installer.sh — stage a portable macOS package from an install tree.
#
# The Linux counterpart of this script (build-linux-installer.sh) is the model;
# three things are different and each one is a way a macOS package fails on a
# machine that is not the one that built it:
#
#   1. The GUI is an .app bundle, not a file in bin/. `cmake --install` puts it
#      at the top of the prefix (install(TARGETS retdec-gui BUNDLE DESTINATION .)).
#   2. Its Qt frameworks and their dependencies are deployed by macdeployqt,
#      which leaves some of them pointing into the build machine's Homebrew
#      prefix. MAC-01 (scripts/ci/check_macos_bundle.py --fix) brings those in
#      and re-signs; this script runs it on the STAGED copy and fails if it
#      cannot make the bundle resolve and verify.
#   3. Anything downloaded through a browser carries com.apple.quarantine, and
#      an ad-hoc signed binary under quarantine does not open at all. The
#      generated install.sh strips it from what it installs, and says so.
#
# Usage:
#   ./scripts/build-macos-installer.sh [options]
#
# Options:
#   --build-dir DIR     CMake binary directory (default: build/linux)
#   --install-dir DIR   cmake --install prefix (default: install/macos)
#   --dist-dir DIR      Output directory for artifacts (default: dist)
#   --version VER       Version tag for file names (default: git describe, else
#                       the project version)
#   --arch ARCH         Override the architecture in file names (default: uname -m)
#   --build             Run cmake --build before install
#   --skip-install      Skip cmake --install (reuse an existing install-dir)
#   --no-bundle         Package the CLI only; do not ship retdec-gui.app
#   --dmg               Also produce a .dmg beside the tarball
#   --dry-run           Print actions without executing
#
# Artifacts:
#   dist/retdec-<version>-macos-<arch>.tar.gz
#   dist/retdec-<version>-macos-<arch>/          (expanded staging tree)
#   dist/retdec-<version>-macos-<arch>.dmg       (when --dmg)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/retdec-env.sh
source "${SCRIPT_DIR}/lib/retdec-env.sh"

BUILD_DIR=""
INSTALL_DIR=""
DIST_DIR=""
VERSION=""
ARCH=""
DO_BUILD=0
SKIP_INSTALL=0
WANT_BUNDLE=1
WANT_DMG=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--build-dir)    BUILD_DIR="$2";   shift 2 ;;
		--install-dir)  INSTALL_DIR="$2"; shift 2 ;;
		--dist-dir)     DIST_DIR="$2";    shift 2 ;;
		--version)      VERSION="$2";     shift 2 ;;
		--arch)         ARCH="$2";        shift 2 ;;
		--build)        DO_BUILD=1;       shift ;;
		--skip-install) SKIP_INSTALL=1;   shift ;;
		--no-bundle)    WANT_BUNDLE=0;    shift ;;
		--dmg)          WANT_DMG=1;       shift ;;
		--dry-run)      DRY_RUN=1;        shift ;;
		-h|--help)      sed -n '2,40p' "$0"; exit 0 ;;
		*) echo "Unknown option: $1" >&2; exit 1 ;;
	esac
done

BUILD_DIR="${BUILD_DIR:-${RETDEC_BUILD_RELEASE}}"
INSTALL_DIR="${INSTALL_DIR:-${RETDEC_ROOT}/install/macos}"
DIST_DIR="${DIST_DIR:-${RETDEC_ROOT}/dist}"
ARCH="${ARCH:-$(uname -m)}"

_run() {
	if [[ "$DRY_RUN" -eq 1 ]]; then
		echo "[dry-run] $*"
	else
		"$@"
	fi
}

_detect_version() {
	if [[ -n "$VERSION" ]]; then echo "$VERSION"; return; fi
	if [[ -f "${INSTALL_DIR}/share/retdec/BUILD-ID" ]]; then
		local _tag
		_tag=$(sed -n '1s/^RetDec \([^ ]*\).*/\1/p' "${INSTALL_DIR}/share/retdec/BUILD-ID" 2>/dev/null || true)
		if [[ -n "$_tag" ]]; then echo "$_tag"; return; fi
	fi
	if command -v git >/dev/null 2>&1 && \
	   git -C "${RETDEC_ROOT}" describe --tags >/dev/null 2>&1; then
		git -C "${RETDEC_ROOT}" describe --tags
		return
	fi
	# The project's own version, which is what CMakeLists.txt falls back to
	# when git describe finds no tag. Never "5.0" out of nowhere.
	sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9.]+).*/\1/p' \
		"${RETDEC_ROOT}/CMakeLists.txt" | head -1
}

_write_install_scripts() {
	local _stage="$1" _ver="$2"

	cat > "${_stage}/install.sh" <<'INSTALL_EOF'
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
			echo "# RetDec (${RETDEC_INSTALLER_VERSION})"
			echo "export PATH=\"${PREFIX}/bin:\${PATH}\""
		} >> "$_rc"
		echo "Appended a PATH line to ${_rc} (open a new shell, or: source ${_rc})"
	fi
fi

echo ""
echo "RetDec installed."
echo "  prefix: ${PREFIX}"
echo "  try:    ${PREFIX}/bin/retdec-decompiler --help"
INSTALL_EOF

	cat > "${_stage}/uninstall.sh" <<'UNINSTALL_EOF'
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
UNINSTALL_EOF

	if [[ "$DRY_RUN" -eq 0 ]]; then
		# BSD sed wants an argument to -i; GNU sed must not get one.
		sed -i '' "s/\${RETDEC_INSTALLER_VERSION}/${_ver}/g" "${_stage}/install.sh" 2>/dev/null || \
			sed -i "s/\${RETDEC_INSTALLER_VERSION}/${_ver}/g" "${_stage}/install.sh"
		chmod +x "${_stage}/install.sh" "${_stage}/uninstall.sh"
	fi
}

_write_readme() {
	local _stage="$1" _ver="$2" _arch="$3" _gui="$4"

	cat > "${_stage}/README" <<EOF
RetDec ${_ver} — macOS ${_arch} portable package
================================================

Contents
--------
  bin/            RetDec command-line tools
  lib/            Shared libraries
  share/retdec/   Support data, signatures, docs, BUILD-ID
  ${_gui}
  install.sh      Copy this tree to /usr/local/retdec or ~/.local/retdec
  uninstall.sh    Remove an install and its PATH line

Quick install
-------------
  tar xzf retdec-${_ver}-macos-${_arch}.tar.gz
  cd retdec-${_ver}-macos-${_arch}
  ./install.sh

  # User-local, add PATH automatically:
  ./install.sh --user --add-path

Run without installing
----------------------
  xattr -dr com.apple.quarantine .
  export PATH="\$(pwd)/bin:\$PATH"
  retdec-decompiler --help

Gatekeeper
----------
These binaries are signed ad-hoc, not with an Apple Developer ID. macOS
attaches com.apple.quarantine to anything a browser downloaded, and refuses to
open a quarantined binary that is not notarised. install.sh removes the
attribute from what it installs; to do it by hand, run the xattr line above on
the extracted directory.

Build from source: see docs/BUILD_REFERENCE.md in the repository.
EOF
}

echo "=== RetDec macOS package ==="
echo "  build-dir:   ${BUILD_DIR}"
echo "  install-dir: ${INSTALL_DIR}"
echo "  dist-dir:    ${DIST_DIR}"
echo "  arch:        ${ARCH}"

if [[ "$DO_BUILD" -eq 1 ]]; then
	echo ""
	echo "--- cmake --build ---"
	_run cmake --build "${BUILD_DIR}" --parallel "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
fi

if [[ "$SKIP_INSTALL" -eq 0 ]]; then
	echo ""
	echo "--- cmake --install → ${INSTALL_DIR} ---"
	_run cmake --install "${BUILD_DIR}" --prefix "${INSTALL_DIR}"
fi

[[ -d "${INSTALL_DIR}/bin" ]] || {
	echo "ERROR: ${INSTALL_DIR}/bin not found — build and cmake --install first." >&2
	exit 1
}

VERSION="$(_detect_version)"
VERSION_SAFE="${VERSION//\//-}"
VERSION_SAFE="${VERSION_SAFE// /-}"

STAGE_NAME="retdec-${VERSION_SAFE}-macos-${ARCH}"
STAGE_DIR="${DIST_DIR}/${STAGE_NAME}"
TARBALL="${DIST_DIR}/${STAGE_NAME}.tar.gz"

echo ""
echo "--- Staging ${STAGE_DIR} ---"
if [[ "$DRY_RUN" -eq 0 ]]; then
	rm -rf "${STAGE_DIR}"
	mkdir -p "${STAGE_DIR}"
fi

for _item in bin lib lib64 share; do
	if [[ -d "${INSTALL_DIR}/${_item}" ]]; then
		_run cp -a "${INSTALL_DIR}/${_item}" "${STAGE_DIR}/"
	fi
done

# ─── The .app bundle ──────────────────────────────────────────────────────────
# install(TARGETS retdec-gui BUNDLE DESTINATION .) puts it at the top of the
# prefix. It is renamed on the way in: the bundle's display name is RetDec, and
# a directory called retdec-gui.app in /Applications would be the only thing in
# there that is not.
GUI_LINE="(no GUI in this package)"
BUNDLE_SRC="${INSTALL_DIR}/retdec-gui.app"
if [[ "$WANT_BUNDLE" -eq 1 && -d "${BUNDLE_SRC}" ]]; then
	echo ""
	echo "--- RetDec.app ---"
	_run cp -a "${BUNDLE_SRC}" "${STAGE_DIR}/RetDec.app"
	# MAC-01 on the staged copy, never on the install tree: what ships is what
	# is checked. --fix brings in the libraries macdeployqt left pointing at
	# this machine's Homebrew prefix, then re-signs, then verifies. A bundle
	# that cannot be made to verify does not get packaged silently.
	_run python3 "${SCRIPT_DIR}/ci/check_macos_bundle.py" --fix "${STAGE_DIR}/RetDec.app"
	GUI_LINE="RetDec.app      The GUI. Open it, or copy it to /Applications"
elif [[ "$WANT_BUNDLE" -eq 1 ]]; then
	echo ""
	echo "NOTE: ${BUNDLE_SRC} is not there; packaging the CLI only."
fi

_write_install_scripts "${STAGE_DIR}" "${VERSION_SAFE}"
_write_readme "${STAGE_DIR}" "${VERSION_SAFE}" "${ARCH}" "${GUI_LINE}"

echo ""
echo "--- Creating tarball ${TARBALL} ---"
_run mkdir -p "${DIST_DIR}"
if [[ "$DRY_RUN" -eq 0 ]]; then
	(
		cd "${DIST_DIR}"
		# COPYFILE_DISABLE keeps bsdtar from writing an AppleDouble ._ entry
		# beside every file that carries extended attributes.
		COPYFILE_DISABLE=1 tar czf "${STAGE_NAME}.tar.gz" "${STAGE_NAME}"
	)
fi

# ─── Optional disk image ──────────────────────────────────────────────────────
if [[ "$WANT_DMG" -eq 1 ]]; then
	DMG="${DIST_DIR}/${STAGE_NAME}.dmg"
	echo ""
	echo "--- Disk image ${DMG} ---"
	# hdiutil create fails with "File exists" rather than overwriting, which is
	# what made macdeployqt's POST_BUILD -dmg break every incremental build.
	_run rm -f "${DMG}"
	_run hdiutil create -quiet -volname "RetDec ${VERSION_SAFE}" \
		-srcfolder "${STAGE_DIR}" -ov -format UDZO "${DMG}"
fi

_publish_release_artifacts() {
	local _ver="$1" _stage="$2"
	local _rel="${RETDEC_ROOT}/releases/macos"
	if [[ "$DRY_RUN" -eq 1 ]]; then
		echo "[dry-run] publish release scripts → ${_rel}"
		return 0
	fi
	mkdir -p "${_rel}"
	cp "${_stage}/install.sh" "${_stage}/uninstall.sh" "${_rel}/"
	chmod +x "${_rel}/install.sh" "${_rel}/uninstall.sh"
}

_publish_release_artifacts "${VERSION_SAFE}" "${STAGE_DIR}"

echo ""
echo "=== macOS package complete ==="
echo "  staging: ${STAGE_DIR}"
echo "  tarball: ${TARBALL}"
[[ "$WANT_DMG" -eq 1 ]] && echo "  dmg:     ${DIST_DIR}/${STAGE_NAME}.dmg"
echo "  scripts: releases/macos/{install,uninstall}.sh"
