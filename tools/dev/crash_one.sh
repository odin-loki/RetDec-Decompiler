#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
DECOMPILER="${DECOMPILER:-$BUILD/src/retdec-decompiler/retdec-decompiler}"
CONFIG="${CONFIG:-$REPO_ROOT/src/retdec-decompiler/decompiler-config.json}"
BIN=${1:-/tmp/coverage_corpus/globals_and_structs_O0}

echo "=== Testing: $BIN ==="
"$DECOMPILER" --output /tmp/crash_test.c --config "$CONFIG" "$BIN" 2>&1
echo "Exit: $?"
