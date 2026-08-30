#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"
cmake --build build/linux --target retdec retdec-neural retdec-neural-tests \
	retdec-retdec-tests retdec-decompiler retdec_algo_recover_tests --parallel 8
./build/linux/tests/neural/retdec-neural-tests --gtest_brief=1
./build/linux/tests/retdec/retdec-retdec-tests \
	--gtest_filter='BuildableSidecars.*:FunctionAnalysisCacheTest.*'
./build/linux/tests/algo_recover/retdec_algo_recover_tests --gtest_brief=1
python3 tests/algorithm_recovery/test_labels.py
python3 scripts/ci/check_link_graph.py --self-test
python3 scripts/ci/check_doc_vs_code.py
echo VERIFY_OK
