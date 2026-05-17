#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
DECOMPILER="${DECOMPILER:-$BUILD/src/retdec-decompiler/retdec-decompiler}"
CONFIG="${CONFIG:-$REPO_ROOT/src/retdec-decompiler/decompiler-config.json}"
CORPUS=/tmp/coverage_corpus

CRASHES=(
    globals_and_structs_O0
    ternary_and_casts_O0
    type_conversions_O0
    simd_like_O0
    global_init_O0
    complex_control_flow_O2
    exception_like_O2
)

for name in "${CRASHES[@]}"; do
    bin="$CORPUS/$name"
    [ -f "$bin" ] || continue
    echo "=== Backtrace: $name ==="
    gdb -batch \
        -ex "set pagination off" \
        -ex "run --output /tmp/gdb_out.c --config $CONFIG $bin" \
        -ex "bt 20" \
        -ex "quit" \
        "$DECOMPILER" 2>&1 | grep -E 'Program received|signal|#[0-9]|assert|abort|SIGABRT|Thread' | head -30
    echo ""
done
