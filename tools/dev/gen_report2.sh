#!/usr/bin/env bash
# Generate focused coverage report for llvmir2hll source + test files ONLY.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRCDIR="${SRCDIR:-$REPO_ROOT}"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
REPORT_DIR="${REPORT_DIR:-/tmp/coverage_report}"
mkdir -p "$REPORT_DIR"
rm -f "$REPORT_DIR"/*.html "$REPORT_DIR"/*.txt

echo "=== Generating HTML coverage report ==="
python3 -m gcovr \
    --root "$SRCDIR" \
    --object-directory "$BUILD/src/llvmir2hll" \
    --object-directory "$BUILD/tests/llvmir2hll" \
    --filter "${SRCDIR}/src/llvmir2hll" \
    --filter "${SRCDIR}/tests/llvmir2hll" \
    --filter "${SRCDIR}/include/retdec/llvmir2hll" \
    --exclude ".*external.*" \
    --html-details "$REPORT_DIR/index.html" \
    --html-title "RetDec llvmir2hll — Full Decompilation Coverage" \
    --html-self-contained \
    --print-summary \
    --txt "$REPORT_DIR/summary.txt" \
    --gcov-executable gcov \
    2>&1 | grep -v '(WARNING)' | grep -v '(INFO) Reading'

echo ""
echo "=== Source-only coverage (src/llvmir2hll) ==="
python3 -m gcovr \
    --root "$SRCDIR" \
    --object-directory "$BUILD/src/llvmir2hll" \
    --filter "${SRCDIR}/src/llvmir2hll" \
    --exclude ".*external.*" \
    --print-summary \
    --gcov-executable gcov \
    2>&1 | grep -E 'lines:|functions:|branches:'

echo ""
echo "=== Coverage Summary ==="
cat "$REPORT_DIR/summary.txt" | grep -E 'TOTAL|File' | head -5

# Copy report to the Windows-accessible location for easy browsing
WIN_REPORT="${WIN_REPORT:-$REPO_ROOT/coverage_report}"
rm -rf "$WIN_REPORT"
mkdir -p "$WIN_REPORT"
cp -r "$REPORT_DIR"/*.html "$WIN_REPORT/" 2>/dev/null
cp "$REPORT_DIR/summary.txt" "$WIN_REPORT/" 2>/dev/null
echo ""
echo "=== Report copied to: coverage_report/ ==="
echo "Files: $(ls $WIN_REPORT/*.html | wc -l) HTML files"
ls -lh "$WIN_REPORT/index.html"
