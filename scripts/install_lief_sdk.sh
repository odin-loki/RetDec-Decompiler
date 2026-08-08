#!/usr/bin/env bash
# install_lief_sdk.sh — Install LIEF C++ SDK when apt has no liblief-dev (e.g. Ubuntu 24.04).
# Usage: bash scripts/install_lief_sdk.sh [--version 1.0.0]
# Sets up deps/lief-sdk/ with CMake package files. Export LIEF_DIR before configuring:
#   export LIEF_DIR="$(pwd)/deps/lief-sdk/lib/cmake/LIEF"
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="1.0.0"
DEST="${ROOT}/deps/lief-sdk"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--version) VERSION="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

arch="$(uname -m)"
case "${arch}" in
	x86_64) LIEF_ARCH="x86_64" ;;
	aarch64|arm64) LIEF_ARCH="aarch64" ;;
	i686|i386) LIEF_ARCH="i686" ;;
	riscv64) LIEF_ARCH="riscv64" ;;
	*) echo "Unsupported arch: ${arch}" >&2; exit 1 ;;
esac

if [[ -f "${DEST}/lib/cmake/LIEF/LIEFConfig.cmake" ]]; then
	echo "LIEF SDK already installed at ${DEST}"
	echo "LIEF_DIR=${DEST}/lib/cmake/LIEF"
	exit 0
fi

TARBALL="LIEF-${VERSION}-Linux-${LIEF_ARCH}.tar.gz"
URL="https://github.com/lief-project/LIEF/releases/download/${VERSION}/${TARBALL}"

echo "==> Downloading ${URL}"
mkdir -p "${DEST}"
tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT
curl -fsSL -o "${tmpdir}/${TARBALL}" "${URL}"
tar -xzf "${tmpdir}/${TARBALL}" -C "${tmpdir}"
# Archives contain a top-level LIEF-x.y.z-Linux-.../ directory.
extracted="$(find "${tmpdir}" -mindepth 1 -maxdepth 1 -type d | head -n1)"
rm -rf "${DEST}"
mkdir -p "${DEST}"
cp -a "${extracted}/." "${DEST}/"

echo "LIEF SDK installed to ${DEST}"
echo "Use:"
echo "  export LIEF_DIR=\"${DEST}/lib/cmake/LIEF\""
echo "  cmake ... -DRETDEC_ENABLE_LIEF=ON -DLIEF_DIR=\"\${LIEF_DIR}\""
