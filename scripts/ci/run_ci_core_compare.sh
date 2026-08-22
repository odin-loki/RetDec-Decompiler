#!/usr/bin/env bash
# Stage decompiler share/ and run ci-core fork vs stock-json compare.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"
DEC="${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler"
if [[ ! -x "${DEC}" ]]; then
	echo "missing ${DEC}" >&2
	exit 1
fi
DEC_DIR="$(cd "$(dirname "${DEC}")" && pwd)"
SHARE_DIR="$(cd "${DEC_DIR}/.." && pwd)/share/retdec"
mkdir -p "${SHARE_DIR}/profiles"
cp -f "${ROOT}/src/retdec-decompiler/decompiler-config.json" "${SHARE_DIR}/"
if [[ -d "${ROOT}/src/retdec-decompiler/profiles" ]]; then
	cp -f "${ROOT}/src/retdec-decompiler/profiles/"*.json "${SHARE_DIR}/profiles/" 2>/dev/null || true
fi
echo "Using ${DEC}"
echo "Staged ${SHARE_DIR}"

SMOKE_DIR="${ROOT}/build/linux/smoke-bs"
rm -rf "${SMOKE_DIR}"
mkdir -p "${SMOKE_DIR}"
RETDEC_EMIT_BUILDABLE=1 RETDEC_PROFILE_JSON=1 \
	"${DEC}" "${ROOT}/tests/algorithm_recovery/corpus/binary_search-gcc-O0" \
	--output "${SMOKE_DIR}/bs.c"
echo "SMOKE_OK"
ls -la "${SMOKE_DIR}"

python3 "${ROOT}/tests/decompilebench/runner.py" \
	--decompiler "${DEC}" \
	--corpus "${ROOT}/tests/decompilebench/corpus" \
	--out "${ROOT}/results/decompilebench-ci-core.json" \
	--limit 9 \
	--emit-buildable-env \
	--stock-json "${ROOT}/results/stock-retdec-docker-ci-core.json" \
	--markdown-out "${ROOT}/results/compare-fork-vs-stock.md"

echo "COMPARE_OK"
