#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
DECOMPILER="${DECOMPILER:-$BUILD/src/retdec-decompiler/retdec-decompiler}"
CONFIG="${CONFIG:-$REPO_ROOT/src/retdec-decompiler/decompiler-config.json}"
BIN=/tmp/coverage_corpus/stack_machine_O0

echo "=== Reproducing segfault on stack_machine_O0 ==="
ulimit -c unlimited
"$DECOMPILER" --output /tmp/sm_crash.c --config "$CONFIG" "$BIN" 2>&1 | tail -30
echo "Exit code: $?"

echo ""
echo "=== Getting backtrace with gdb ==="
gdb -batch \
    -ex "run --output /tmp/sm_gdb.c --config $CONFIG $BIN" \
    -ex "bt full" \
    -ex "quit" \
    "$DECOMPILER" 2>&1 | grep -A 50 'Thread 1\|Program received\|Backtrace\|#[0-9]' | head -60
