#!/usr/bin/env bash
# Stage the full algorithm-recovery stand-in corpus and compare fork vs stock JSON.
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

bash "${ROOT}/scripts/fetch_decompilebench_corpus.sh" --profile full

STOCK_JSON="${ROOT}/results/stock-retdec-docker-full.json"
STOCK_ARGS=()
if [[ -f "${STOCK_JSON}" ]]; then
	STOCK_ARGS=(--stock-json "${STOCK_JSON}")
fi

python3 "${ROOT}/tests/decompilebench/runner.py" \
	--decompiler "${DEC}" \
	--corpus "${ROOT}/tests/decompilebench/corpus" \
	--out "${ROOT}/results/decompilebench-full.json" \
	--emit-buildable-env \
	"${STOCK_ARGS[@]}" \
	--markdown-out "${ROOT}/results/compare-fork-vs-stock-full.md"

echo "FULL_COMPARE_OK"
