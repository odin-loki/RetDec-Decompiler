#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"
cmake --build build/linux --target retdec-retdec-tests --parallel 8
./build/linux/tests/retdec/retdec-retdec-tests --gtest_filter='BuildableSidecars.*:FunctionAnalysisCacheTest.*'
bash scripts/ci/run_skip_semantic_ab.sh
bash scripts/ci/run_neural_compilable_smoke.sh
echo POST_BUILD_OK
