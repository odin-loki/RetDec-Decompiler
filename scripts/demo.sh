#!/usr/bin/env bash
# demo.sh — Five-minute product demo (Part 12.5).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER="$(grep -E '^[[:space:]]*VERSION[[:space:]]' "${ROOT}/CMakeLists.txt" | head -1 | sed -E 's/.*VERSION[[:space:]]+([0-9.]+).*/\1/')"

echo "==> RetDec Imortek v${VER} demo"
echo "==> Offline assertion"
export RETDEC_NO_NETWORK=1
echo "RETDEC_NO_NETWORK=1 (air-gapped mode)"

echo "==> Licence files"
for f in LICENSE LICENSE-AGPL LICENSE-COMMERCIAL NOTICE; do
	test -f "${ROOT}/${f}"
done

DEC=""
for candidate in \
	"${ROOT}/build/linux/bin/retdec-decompiler" \
	"${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler" \
	"$(command -v retdec-decompiler 2>/dev/null || true)"; do
	if [[ -n "${candidate}" && -x "${candidate}" ]]; then
		DEC="${candidate}"
		break
	fi
done
[[ -n "${DEC}" ]] || { echo "retdec-decompiler not found"; exit 1; }

echo "==> Build algorithm corpus (best-effort)"
bash "${ROOT}/scripts/build_algorithm_corpus.sh" 2>/dev/null || true

CORPUS="${ROOT}/tests/algorithm_recovery/corpus"
SAMPLE="${ROOT}/tests/regression/samples/pe-x86-32.exe"
if [[ ! -f "${SAMPLE}" ]]; then
	SAMPLE="$(find "${ROOT}/tests" -name '*.exe' -type f 2>/dev/null | head -1 || true)"
fi
if [[ -z "${SAMPLE}" || ! -f "${SAMPLE}" ]]; then
	if [[ -d "${CORPUS}" ]]; then
		SAMPLE="$(find "${CORPUS}" -type f | head -1 || true)"
	fi
fi
[[ -n "${SAMPLE}" && -f "${SAMPLE}" ]] || { echo "No sample binary"; exit 1; }

OUT="${ROOT}/build/demo-out.c"
"${DEC}" "${SAMPLE}" --output "${OUT}"
test -s "${OUT}"
echo "Decompiled ${SAMPLE} -> ${OUT} ($(wc -c < "${OUT}") bytes)"

if [[ -d "${CORPUS}" ]]; then
	echo "==> DecompileBench on algorithm corpus"
	python3 "${ROOT}/tests/decompilebench/runner.py" \
		--decompiler "${DEC}" \
		--corpus "${CORPUS}" \
		--out "${ROOT}/build/demo-decompilebench.json" \
		--opts O0 || true
fi

echo "==> Benchmark tables"
bash "${ROOT}/scripts/run_benchmarks.sh" --compare 2026-08 || true

echo "==> Doctor"
bash "${ROOT}/scripts/doctor.sh" | tail -n 3

echo "Demo complete."
