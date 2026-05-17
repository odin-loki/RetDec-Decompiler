#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
DECOMPILER="${DECOMPILER:-$BUILD/src/retdec-decompiler/retdec-decompiler}"
BINARY=/tmp/ftest/ll

for OPT in CopyPropagationOptimizer SimpleCopyPropagationOptimizer DeadLocalAssignOptimizer BitShiftOptimizer DerefAddressOptimizer SimplifyArithmExprOptimizer; do
    echo "=== Disabling: $OPT"
    "$DECOMPILER" "$BINARY" -s --backend-disabled-opts "$OPT" -o "/tmp/ll_test_${OPT}.c" 2>/dev/null
    result=$(sed -n '/function_11af @/,/^}/p' "/tmp/ll_test_${OPT}.c" | grep 'uint64_t.*v1 + 8\|v1 += 8')
    if echo "$result" | grep -q 'uint64_t'; then
        echo "  -> CORRECT (has dereference)"
    else
        echo "  -> BUG (still has += 8)"
    fi
done
