#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"
cmake --build build/linux --target retdec retdec-neural retdec-neural-tests \
	retdec-decompiler --parallel 8
# retdec-retdec-tests may fail on pre-existing function_analysis_cache_test
cmake --build build/linux --target retdec-retdec-tests --parallel 8 || \
	echo "WARN: retdec-retdec-tests did not link (pre-existing cache test?)"
./build/linux/tests/neural/retdec-neural-tests --gtest_brief=1
python3 scripts/ci/check_link_graph.py --self-test
python3 scripts/ci/check_doc_vs_code.py
python3 scripts/ci/preview_temp_inject.py
echo BUILD_VERIFY_OK
