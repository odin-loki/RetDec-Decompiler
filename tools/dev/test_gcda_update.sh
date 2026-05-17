#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
DECOMPILER="${DECOMPILER:-$BUILD/src/retdec-decompiler/retdec-decompiler}"
LLVMIR2HLL_OBJ="$BUILD/src/llvmir2hll/CMakeFiles/llvmir2hll.dir"
GCDA="$LLVMIR2HLL_OBJ/optimizer/optimizer_manager.cpp.gcda"
CONFIG="${CONFIG:-$REPO_ROOT/src/retdec-decompiler/decompiler-config.json}"

before=$(stat -c '%Y' "$GCDA")
echo "gcda mtime before: $before"
echo "gcda size before: $(stat -c '%s' $GCDA)"

# Run decompiler
"$DECOMPILER" --arch x86-64 --format elf \
    --output /tmp/test_cov_check.c \
    --config "$CONFIG" \
    /tmp/coverage_corpus/factorial_O0 > /dev/null 2>&1
ret=$?

after=$(stat -c '%Y' "$GCDA")
echo "gcda mtime after: $after"
echo "gcda size after: $(stat -c '%s' $GCDA)"
echo "decompiler exit code: $ret"

if [ "$before" -eq "$after" ]; then
    echo "ERROR: gcda NOT updated — coverage instrumentation is not writing data!"
    echo ""
    echo "Checking if decompiler binary has gcov symbols..."
    nm "$DECOMPILER" 2>/dev/null | grep -i gcov | head -5 || echo "  No gcov symbols in decompiler binary"
    echo ""
    echo "Checking llvmir2hll.a for gcov symbols..."
    nm "$BUILD/src/llvmir2hll/libretdec-llvmir2hll.a" 2>/dev/null | grep -i gcov | head -5
else
    echo "OK: gcda updated by decompiler run"
fi

# Also show what gcov now reports
cd "$LLVMIR2HLL_OBJ"
gcov -b optimizer/optimizer_manager.cpp.gcda 2>&1 | grep -E 'Lines executed|File.*optimizer_manager'
