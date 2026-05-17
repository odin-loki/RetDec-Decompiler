#!/usr/bin/env bash
# Debug why optimizer_manager shows 0% despite decompiler running
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
DECOMPILER="${DECOMPILER:-$BUILD/src/retdec-decompiler/retdec-decompiler}"
CONFIG="${CONFIG:-$REPO_ROOT/src/retdec-decompiler/decompiler-config.json}"
GCDA_DIR="$BUILD/src/llvmir2hll/CMakeFiles/llvmir2hll.dir"

echo "=== Clearing all gcda files ==="
find "$BUILD/src/llvmir2hll" -name '*.gcda' -delete
find "$BUILD/src/retdec-decompiler" -name '*.gcda' -delete 2>/dev/null

echo ""
echo "=== Running decompiler with GCOV_ERROR_FILE ==="
GCOV_ERROR_FILE=/tmp/gcov_errors.txt \
"$DECOMPILER" --arch x86-64 --format elf \
    --output /tmp/debug_cov.c \
    --config "$CONFIG" \
    /tmp/coverage_corpus/factorial_O0 2>/dev/null
echo "Decompiler exit code: $?"

echo ""
echo "=== GCOV errors (if any) ==="
cat /tmp/gcov_errors.txt 2>/dev/null || echo "(no errors)"

echo ""
echo "=== gcda files created after decompiler run ==="
find "$BUILD/src/llvmir2hll" -name '*.gcda' | wc -l
# Check optimizer_manager specifically
GCDA="$GCDA_DIR/optimizer/optimizer_manager.cpp.gcda"
if [ -f "$GCDA" ]; then
    echo "optimizer_manager.gcda EXISTS ($(stat -c '%s' $GCDA) bytes)"
    # Run gcov and get the coverage
    cd "$GCDA_DIR"
    gcov -b optimizer/optimizer_manager.cpp.gcda 2>&1 | grep -E 'optimizer_manager|Lines executed' | head -5
else
    echo "optimizer_manager.gcda DOES NOT EXIST — binary not writing gcda!"
fi

echo ""
echo "=== Checking where decompiler binary writes gcda files ==="
# The gcda file path is embedded in the binary
strings "$DECOMPILER" | grep 'optimizer_manager' | head -3
