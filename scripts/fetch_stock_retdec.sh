#!/usr/bin/env bash
# fetch_stock_retdec.sh — Download upstream RetDec v5.0 SDK for reference builds.
#
# Note: RetDec-v5.0-Linux-Release.tar.xz is an SDK (headers/libs), not a prebuilt
# retdec-decompiler binary. For live two-column benchmarks, install the Docker
# image `remnux/retdec` (stock v5.0; `retdec/retdec:v5.0` does not exist) or set:
#   export RETDEC_STOCK_DECOMPILER=/path/to/retdec-decompiler
#
# Usage: bash scripts/fetch_stock_retdec.sh [--version 5.0]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="5.0"
DEST="${ROOT}/deps/stock-retdec"
URL="https://github.com/avast/retdec/releases/download/v${VERSION}/RetDec-v${VERSION}-Linux-Release.tar.xz"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--version) VERSION="$2"; URL="https://github.com/avast/retdec/releases/download/v${VERSION}/RetDec-v${VERSION}-Linux-Release.tar.xz"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ -n "${RETDEC_STOCK_DECOMPILER:-}" && -x "${RETDEC_STOCK_DECOMPILER}" ]]; then
	echo "Using RETDEC_STOCK_DECOMPILER=${RETDEC_STOCK_DECOMPILER}"
	exit 0
fi

DEC="$(find "${DEST}" -name retdec-decompiler -type f 2>/dev/null | head -n1 || true)"
if [[ -n "${DEC}" && -x "${DEC}" ]]; then
	echo "Stock RetDec already at ${DEC}"
	exit 0
fi

if [[ -f "${DEST}/retdec/retdec/retdec.h" || -d "${DEST}/retdec" ]]; then
	echo "SDK already at ${DEST} (no retdec-decompiler binary in release tarball)."
	echo "Set RETDEC_STOCK_DECOMPILER to a v5.0 binary for two-column compare."
	exit 2
fi

echo "==> Downloading ${URL}"
tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT
curl -fsSL -o "${tmpdir}/retdec.tar.xz" "${URL}"
rm -rf "${DEST}"
mkdir -p "${DEST}"
tar -xJf "${tmpdir}/retdec.tar.xz" -C "${tmpdir}"
extracted="$(find "${tmpdir}" -mindepth 1 -maxdepth 1 -type d | head -n1)"
cp -a "${extracted}/." "${DEST}/"

DEC="$(find "${DEST}" -name retdec-decompiler -type f 2>/dev/null | head -n1 || true)"
if [[ -n "${DEC}" && -x "${DEC}" ]]; then
	echo "Stock RetDec v${VERSION}: ${DEC}"
	exit 0
fi

echo "Downloaded SDK to ${DEST} — no retdec-decompiler binary in official Linux release."
echo "For benchmarks: bash scripts/run_stock_retdec_docker.sh --profile ci-core"
exit 2
