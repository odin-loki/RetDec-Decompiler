#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"
cmake --build build/linux --target retdec retdec-retdec-tests retdec-decompiler --parallel 8
./build/linux/tests/retdec/retdec-retdec-tests --gtest_filter='BuildableSidecars.*'
bash scripts/ci/run_ci_core_compare.sh
echo REBENCH_OK
