#!/usr/bin/env bash
# Build Linux tarball in dist/ and sync install scripts to releases/linux/.
# Intended for WSL/Linux: bash scripts/build-linux-release-artifact.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${SCRIPT_DIR}/.." && pwd)"
VERSION="${1:-5.0}"

# Prefer native Linux filesystem for speed when repo is on /mnt/c.
if [[ "${REPO}" == /mnt/* ]]; then
	WORK="${HOME}/retdec-linux-build"
	mkdir -p "${WORK}"
	rsync -a --delete \
		--exclude build --exclude 'build-*' --exclude install --exclude dist --exclude .git \
		"${REPO}/" "${WORK}/"
	cd "${WORK}"
else
	cd "${REPO}"
fi

echo "==> fetch-large-files"
bash scripts/fetch-large-files.sh

if [[ ! -f build/linux/CMakeCache.txt ]]; then
	echo "==> cmake --preset full-linux-release"
	cmake --preset full-linux-release -DRETDEC_ENABLE_CUDA_ACCEL=OFF
fi

echo "==> cmake --build retdec-decompiler retdec-gui"
cmake --build build/linux --parallel "$(nproc 2>/dev/null || echo 4)" \
	--target retdec-decompiler retdec-gui

echo "==> cmake --install"
cmake --install build/linux --prefix install/linux

echo "==> package tarball"
chmod +x scripts/build-linux-installer.sh
./scripts/build-linux-installer.sh --skip-install --version "${VERSION}"

DEST="${REPO}/releases/linux"
mkdir -p "${DEST}"
cp -f "dist/retdec-${VERSION}-linux-x64/install.sh" \
	"dist/retdec-${VERSION}-linux-x64/uninstall.sh" "${DEST}/" 2>/dev/null || true

echo "==> Tarball: dist/retdec-${VERSION}-linux-x64.tar.gz"
echo "==> Scripts: ${DEST}/install.sh ${DEST}/uninstall.sh"
