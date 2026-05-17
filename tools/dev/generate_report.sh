#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
SRCDIR="${SRCDIR:-$REPO_ROOT}"
LLVMIR2HLL_SRC=$SRCDIR/src/llvmir2hll
LLVMIR2HLL_BUILD=$BUILD/src/llvmir2hll
TESTS_BUILD=$BUILD/tests/llvmir2hll
REPORT_DIR=/tmp/coverage_report
mkdir -p "$REPORT_DIR"

echo "=== Generating HTML coverage report ==="

python3 -m gcovr \
    --root "$SRCDIR" \
    --filter "${LLVMIR2HLL_SRC}" \
    --object-directory "${LLVMIR2HLL_BUILD}" \
    --object-directory "${TESTS_BUILD}" \
    --html-details "${REPORT_DIR}/index.html" \
    --html-title "RetDec llvmir2hll — Full Decompilation Coverage" \
    --html-self-contained \
    --print-summary \
    --txt "${REPORT_DIR}/summary.txt" \
    --gcov-executable gcov \
    2>&1

echo ""
echo "=== Summary ==="
cat "${REPORT_DIR}/summary.txt" 2>/dev/null || echo "(no summary)"

echo ""
echo "=== Report files ==="
ls -lh "${REPORT_DIR}/"
echo ""
echo "HTML report: ${REPORT_DIR}/index.html"
