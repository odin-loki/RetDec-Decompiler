#!/usr/bin/env bash
# build-linux-installer.sh — Install RetDec with cmake --install and stage a
# portable Linux tarball (plus optional AppImage / .deb).
#
# Usage:
#   ./scripts/build-linux-installer.sh [options]
#
# Options:
#   --build-dir DIR     CMake binary directory (default: build/linux)
#   --install-dir DIR   cmake --install prefix (default: install/linux)
#   --dist-dir DIR      Output directory for artifacts (default: dist)
#   --version VER       Override version tag (default: git describe or 5.0)
#   --preset PRESET     CMake preset used with --build (default: full-linux-release)
#   --build             Run cmake --build before install
#   --skip-install      Skip cmake --install (reuse existing install-dir)
#   --dry-run           Print actions without executing
#
# Environment:
#   APPIMAGE=1          Also invoke scripts/make-appimage.sh
#   FPM=1               Require fpm and build a .deb (otherwise best-effort)
#
# Artifacts:
#   dist/retdec-<version>-linux-x64.tar.gz
#   dist/retdec-<version>-linux-x64/          (expanded staging tree)
#   dist/retdec-<version>-x86_64.AppImage     (when APPIMAGE=1)
#   dist/retdec_<version>_amd64.deb           (when fpm is available)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/retdec-env.sh
source "${SCRIPT_DIR}/lib/retdec-env.sh"

BUILD_DIR=""
INSTALL_DIR=""
DIST_DIR=""
VERSION=""
PRESET="${RETDEC_CMAKE_PRESET_LINUX_REL:-full-linux-release}"
DO_BUILD=0
SKIP_INSTALL=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--build-dir)   BUILD_DIR="$2";    shift 2 ;;
		--install-dir) INSTALL_DIR="$2";  shift 2 ;;
		--dist-dir)    DIST_DIR="$2";     shift 2 ;;
		--version)     VERSION="$2";      shift 2 ;;
		--preset)      PRESET="$2";       shift 2 ;;
		--build)       DO_BUILD=1;        shift ;;
		--skip-install) SKIP_INSTALL=1;   shift ;;
		--dry-run)     DRY_RUN=1;         shift ;;
		-h|--help)
			sed -n '2,26p' "$0"
			exit 0
			;;
		*) echo "Unknown option: $1" >&2; exit 1 ;;
	esac
done

BUILD_DIR="${BUILD_DIR:-${RETDEC_BUILD_RELEASE}}"
INSTALL_DIR="${INSTALL_DIR:-${RETDEC_ROOT}/install/linux}"
DIST_DIR="${DIST_DIR:-${RETDEC_ROOT}/dist}"

_run() {
	if [[ "$DRY_RUN" -eq 1 ]]; then
		echo "[dry-run] $*"
	else
		"$@"
	fi
}

_detect_version() {
	if [[ -n "$VERSION" ]]; then
		echo "$VERSION"
		return
	fi
	if [[ -f "${INSTALL_DIR}/share/retdec/BUILD-ID" ]]; then
		local _tag
		_tag=$(sed -n '1s/^RetDec \([^ ]*\).*/\1/p' "${INSTALL_DIR}/share/retdec/BUILD-ID" 2>/dev/null || true)
		if [[ -n "$_tag" ]]; then
			echo "$_tag"
			return
		fi
	fi
	if command -v git >/dev/null 2>&1 && git -C "${RETDEC_ROOT}" describe --tags --always >/dev/null 2>&1; then
		git -C "${RETDEC_ROOT}" describe --tags --always
		return
	fi
	echo "5.0"
}

_write_install_scripts() {
	local _stage="$1"
	local _ver="$2"

	cat > "${_stage}/install.sh" <<'INSTALL_EOF'
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
	_snippet="# RetDec (${RETDEC_INSTALLER_VERSION})"
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
INSTALL_EOF

	cat > "${_stage}/uninstall.sh" <<'UNINSTALL_EOF'
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
UNINSTALL_EOF

	# Embed version for PATH comment in install.sh.
	if [[ "$DRY_RUN" -eq 0 ]]; then
		sed -i "s/\${RETDEC_INSTALLER_VERSION}/${_ver}/g" "${_stage}/install.sh" 2>/dev/null || \
			sed -i '' "s/\${RETDEC_INSTALLER_VERSION}/${_ver}/g" "${_stage}/install.sh"
		chmod +x "${_stage}/install.sh" "${_stage}/uninstall.sh"
	fi
}

_write_readme() {
	local _stage="$1"
	local _ver="$2"

	cat > "${_stage}/README" <<EOF
RetDec ${_ver} — Linux x86_64 portable package
================================================

Contents
--------
  bin/            RetDec CLI tools and retdec-gui
  lib/            Shared libraries (RPATH-friendly layout)
  share/retdec/   Support data, signatures, docs, BUILD-ID
  install.sh      Copy this tree to /opt/retdec or ~/.local/retdec
  uninstall.sh    Remove an install and optional PATH snippet

Quick install
-------------
  tar xzf retdec-${_ver}-linux-x64.tar.gz
  cd retdec-${_ver}-linux-x64
  chmod +x install.sh uninstall.sh
  ./install.sh

  # User-local, add PATH automatically:
  ./install.sh --user --add-path

  # System-wide (sudo when needed):
  ./install.sh --prefix /opt/retdec --add-path

Run without installing
----------------------
  export PATH="\$(pwd)/bin:\$PATH"
  retdec-decompiler --help

Optional formats
----------------
  AppImage: set APPIMAGE=1 when running scripts/build-linux-installer.sh
  .deb:     install fpm (https://fpm.readthedocs.io/) and re-run the builder,
            or use: gem install --no-document fpm

Build from source: see docs/INSTALL_LINUX.md in the repository.
EOF
}

echo "=== RetDec Linux Installer ==="
echo "  build-dir:   ${BUILD_DIR}"
echo "  install-dir: ${INSTALL_DIR}"
echo "  dist-dir:    ${DIST_DIR}"
echo "  preset:      ${PRESET}"

# ─── Optional build ───────────────────────────────────────────────────────────
if [[ "$DO_BUILD" -eq 1 ]]; then
	echo ""
	echo "--- cmake --build (${PRESET}) ---"
	_run cmake --build "${BUILD_DIR}" --parallel "$(nproc 2>/dev/null || echo 4)"
fi

# ─── cmake --install ──────────────────────────────────────────────────────────
if [[ "$SKIP_INSTALL" -eq 0 ]]; then
	echo ""
	echo "--- cmake --install → ${INSTALL_DIR} ---"
	_run cmake --install "${BUILD_DIR}" --prefix "${INSTALL_DIR}"
fi

[[ -d "${INSTALL_DIR}/bin" ]] || {
	echo "ERROR: ${INSTALL_DIR}/bin not found — run a Release/Debug build and cmake --install first." >&2
	exit 1
}

VERSION="$(_detect_version)"
# Sanitize version for filenames (no slashes/spaces).
VERSION_SAFE="${VERSION//\//-}"
VERSION_SAFE="${VERSION_SAFE// /-}"

STAGE_NAME="retdec-${VERSION_SAFE}-linux-x64"
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

_write_install_scripts "${STAGE_DIR}" "${VERSION_SAFE}"
_write_readme "${STAGE_DIR}" "${VERSION_SAFE}"

echo ""
echo "--- Creating tarball ${TARBALL} ---"
_run mkdir -p "${DIST_DIR}"
if [[ "$DRY_RUN" -eq 0 ]]; then
	(
		cd "${DIST_DIR}"
		tar czf "${STAGE_NAME}.tar.gz" "${STAGE_NAME}"
	)
fi

# ─── Optional AppImage ────────────────────────────────────────────────────────
if [[ "${APPIMAGE:-0}" == "1" ]]; then
	echo ""
	echo "--- AppImage (scripts/make-appimage.sh) ---"
	APPIMAGE_OUT="${DIST_DIR}/retdec-${VERSION_SAFE}-x86_64.AppImage"
	_run "${SCRIPT_DIR}/make-appimage.sh" \
		--install-dir "${INSTALL_DIR}" \
		--out "${APPIMAGE_OUT}" \
		--version "${VERSION_SAFE}"
fi

# ─── Optional .deb via fpm ────────────────────────────────────────────────────
_build_deb() {
	local _deb="${DIST_DIR}/retdec_${VERSION_SAFE}_amd64.deb"
	echo ""
	echo "--- Debian package (${_deb}) ---"
	# fpm maps the staged tree onto /opt/retdec; install the gem if missing:
	#   gem install --no-document fpm
	_run fpm -s dir -t deb \
		-n retdec \
		-v "${VERSION_SAFE}" \
		--architecture amd64 \
		--description "RetDec retargetable machine-code decompiler" \
		--url "https://github.com/avast/retdec" \
		--license MIT \
		-C "${STAGE_DIR}" \
		--prefix /opt/retdec \
		-p "${DIST_DIR}/" \
		.
}

if [[ "${FPM:-0}" == "1" ]]; then
	command -v fpm >/dev/null 2>&1 || {
		echo "ERROR: FPM=1 but fpm not found in PATH." >&2
		echo "Install: gem install --no-document fpm" >&2
		exit 1
	}
	_build_deb
elif command -v fpm >/dev/null 2>&1; then
	_build_deb
else
	echo ""
	echo "--- .deb skipped (fpm not in PATH) ---"
	echo "  Install effing package management (fpm) to produce .deb artifacts:"
	echo "    gem install --no-document fpm"
	echo "  Or re-run with: FPM=1 ./scripts/build-linux-installer.sh ..."
fi

echo ""
echo "=== Linux installer complete ==="
echo "  staging:  ${STAGE_DIR}"
echo "  tarball:  ${TARBALL}"
[[ "${APPIMAGE:-0}" == "1" ]] && echo "  appimage: ${DIST_DIR}/retdec-${VERSION_SAFE}-x86_64.AppImage"
command -v fpm >/dev/null 2>&1 && echo "  deb:      ${DIST_DIR}/retdec_${VERSION_SAFE}_amd64.deb (if fpm succeeded)"

_publish_release_artifacts() {
	local _ver="$1"
	local _stage="$2"
	local _tarball="$3"
	local _rel="${RETDEC_ROOT}/releases/linux"
	if [[ "$DRY_RUN" -eq 1 ]]; then
		echo "[dry-run] publish release scripts → ${_rel}"
		return 0
	fi
	mkdir -p "${_rel}"
	cp "${_stage}/install.sh" "${_stage}/uninstall.sh" "${_rel}/"
	chmod +x "${_rel}/install.sh" "${_rel}/uninstall.sh"
	if [[ -f "${_tarball}" ]]; then
		cp "${_tarball}" "${_rel}/"
	fi
	local _version_file="${RETDEC_ROOT}/releases/VERSION"
	{
		echo "version=${_ver}"
		echo "linux_tarball=releases/linux/$(basename "${_tarball}")"
		echo "linux_install=releases/linux/install.sh"
		echo "updated=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date -u +%Y-%m-%dT%H:%M:%SZ)"
	} > "${_version_file}"
	echo "  releases: ${_rel}"
}

_publish_release_artifacts "${VERSION_SAFE}" "${STAGE_DIR}" "${TARBALL}"
