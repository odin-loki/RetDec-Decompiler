#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
DECOMPILER="${DECOMPILER:-$BUILD/src/retdec-decompiler/retdec-decompiler}"
CONFIG="${CONFIG:-$REPO_ROOT/src/retdec-decompiler/decompiler-config.json}"
CORPUS=/tmp/coverage_corpus

echo "=== Verifying previously crashing binaries ==="
ok=0; fail=0
for name in globals_and_structs_O0 globals_and_structs_O2 \
            ternary_and_casts_O0 ternary_and_casts_O2 \
            type_conversions_O0 type_conversions_O2 \
            simd_like_O0 simd_like_O2 \
            global_init_O0 global_init_O2 \
            complex_control_flow_O2 \
            exception_like_O2; do
    bin="$CORPUS/$name"
    if "$DECOMPILER" --output "/tmp/${name}_out.c" --config "$CONFIG" "$bin" > /dev/null 2>&1; then
        echo "  OK: $name"
        ok=$((ok+1))
    else
        echo "  FAIL: $name"
        # Show brief error
        "$DECOMPILER" --output "/tmp/${name}_out.c" --config "$CONFIG" "$bin" 2>&1 | grep -E 'assert|Assertion|error' | head -2
        fail=$((fail+1))
    fi
done
echo ""
echo "Results: OK=$ok  FAIL=$fail"
