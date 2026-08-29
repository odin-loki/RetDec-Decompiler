#!/usr/bin/env bash
# demo_v1.0.0.sh — smoke demo for current CMake VERSION (not v1.0.0; script reads VER).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER="$(grep -E '^[[:space:]]*VERSION[[:space:]]' "${ROOT}/CMakeLists.txt" | head -1 | sed -E 's/.*VERSION[[:space:]]+([0-9.]+).*/\1/')"

echo "==> RetDec Imortek v${VER} demo"
echo "==> Licence files"
test -f "${ROOT}/LICENSE"
test -f "${ROOT}/LICENSE-AGPL"
test -f "${ROOT}/LICENSE-COMMERCIAL"
test -f "${ROOT}/NOTICE"

echo "==> Neural mock (no model required)"
if [[ -x "${ROOT}/build/linux/bin/retdec-decompiler" ]]; then
  DEC="${ROOT}/build/linux/bin/retdec-decompiler"
elif command -v retdec-decompiler >/dev/null 2>&1; then
  DEC="$(command -v retdec-decompiler)"
else
  echo "retdec-decompiler not found — build with cmake --preset full-linux-debug first"
  exit 1
fi

SAMPLE="${ROOT}/tests/regression/samples/pe-x86-32.exe"
if [[ ! -f "${SAMPLE}" ]]; then
  SAMPLE="$(find "${ROOT}/tests" -name '*.exe' -type f | head -1 || true)"
fi
if [[ -z "${SAMPLE}" || ! -f "${SAMPLE}" ]]; then
  echo "No sample binary found under tests/"
  exit 1
fi

OUT="${ROOT}/build/demo-out.c"
"${DEC}" "${SAMPLE}" --output "${OUT}"
test -s "${OUT}"
echo "Decompiled ${SAMPLE} -> ${OUT} ($(wc -c < "${OUT}") bytes)"

echo "==> Benchmark scaffolds"
python3 "${ROOT}/tests/decompilebench/runner.py" \
  --decompiler "${DEC}" \
  --corpus "$(dirname "${SAMPLE}")" \
  --out "${ROOT}/build/demo-decompilebench.json" \
  --opts O0 || true

echo "Demo complete."
