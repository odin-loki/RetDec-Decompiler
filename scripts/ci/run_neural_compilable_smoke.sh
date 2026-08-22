#!/usr/bin/env bash
# Opt-in mock neural path: emit a cc -fsyntax-only translation unit.
# Does not execute decompiled C. Requires RETDEC_NEURAL_FORCE_MOCK.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEC="${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler"
BIN="${ROOT}/tests/algorithm_recovery/corpus/memcpy_loop-gcc-O0"
OUTDIR="${ROOT}/build/linux/neural-smoke"
mkdir -p "${OUTDIR}"
DUMMY="${OUTDIR}/mock.gguf"
printf 'GGUF' > "${DUMMY}"
export RETDEC_NEURAL_REFINE=1
export RETDEC_NEURAL_MODEL="${DUMMY}"
export RETDEC_NEURAL_ALLOW_UNVERIFIED=1
export RETDEC_NEURAL_FORCE_MOCK=1
export RETDEC_NEURAL_MOCK_EMIT_C=1
export RETDEC_NEURAL_REQUIRE_COMPILE=1
export RETDEC_NEURAL_MAX_TOKENS=64
"${DEC}" "${BIN}" --output "${OUTDIR}/memcpy.c"
ls -la "${OUTDIR}"
if [[ -f "${OUTDIR}/memcpy.c.refined.c" ]]; then
	gcc -fsyntax-only -std=gnu11 -w "${OUTDIR}/memcpy.c.refined.c"
	echo "NEURAL_REFINED_TU_VALID=1"
else
	echo "NEURAL_REFINED_TU_VALID=0 (no sidecar — check manifest)"
	cat "${OUTDIR}/memcpy.c.refinement-manifest.json" 2>/dev/null || true
	exit 1
fi
