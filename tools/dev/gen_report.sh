#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRCDIR="${SRCDIR:-$REPO_ROOT}"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
REPORT_DIR="${REPORT_DIR:-/tmp/coverage_report}"
mkdir -p "$REPORT_DIR"

python3 -m gcovr \
    --root "$SRCDIR" \
    --filter "${SRCDIR}/src/llvmir2hll" \
    --filter "${SRCDIR}/include/retdec/llvmir2hll" \
    --object-directory "$BUILD/src/llvmir2hll" \
    --object-directory "$BUILD/tests/llvmir2hll" \
    --html-details "$REPORT_DIR/index.html" \
    --html-title "RetDec llvmir2hll — Full Decompilation Coverage" \
    --html-self-contained \
    --print-summary \
    --txt "$REPORT_DIR/summary.txt" \
    --gcov-executable gcov \
    2>&1

echo ""
echo "Report files:"
ls -lh "$REPORT_DIR/" | head -5
echo ""
echo "Summary:"
cat "$REPORT_DIR/summary.txt"
