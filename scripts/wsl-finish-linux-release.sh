#!/usr/bin/env bash
# Finish Linux release build from WSL (avoids PowerShell $ expansion issues).
set -euo pipefail

REPO="/mnt/c/Users/odinl/OneDrive/Desktop/RetDec"
WORK="${HOME}/retdec-linux-build"
VERSION="${1:-5.0}"

if [[ ! -d "${WORK}/build/linux" ]]; then
	echo "Missing ${WORK}/build/linux — run build-linux-release-artifact.sh first." >&2
	exit 1
fi

echo "==> Syncing source fixes from repo"
cp "${REPO}/src/retdec/semantic_recovery_export.cpp" "${WORK}/src/retdec/"
cp "${REPO}/src/gui/theme.cpp" "${WORK}/src/gui/"
cp "${REPO}/src/gui/panels/tri_pane_code_view.cpp" "${WORK}/src/gui/panels/"
cp "${REPO}/include/retdec/serdes/semantic_detection.h" "${WORK}/include/retdec/serdes/"
cp "${REPO}/src/serdes/semantic_detection.cpp" \
	"${REPO}/src/serdes/function.cpp" \
	"${REPO}/src/serdes/CMakeLists.txt" \
	"${WORK}/src/serdes/"

cd "${WORK}"

echo "==> Building retdec-decompiler and retdec-gui"
cmake --build build/linux --parallel "$(nproc)" \
	--target retdec-decompiler retdec-gui

echo "==> Installing to install/linux"
cmake --install build/linux --prefix install/linux

echo "==> Packaging tarball"
chmod +x scripts/build-linux-installer.sh
./scripts/build-linux-installer.sh --skip-install --version "${VERSION}"

echo "==> Syncing install scripts to ${REPO}/releases/linux"
mkdir -p "${REPO}/releases/linux"
cp -f "dist/retdec-${VERSION}-linux-x64/install.sh" \
	"dist/retdec-${VERSION}-linux-x64/uninstall.sh" \
	"${REPO}/releases/linux/"

ls -lh "dist/retdec-${VERSION}-linux-x64.tar.gz"
echo "LINUX_BUILD_DONE"
