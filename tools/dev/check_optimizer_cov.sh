#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
OBJ="${OBJ:-$BUILD/src/llvmir2hll/CMakeFiles/llvmir2hll.dir}"
SRCDIR="${SRCDIR:-$REPO_ROOT/src/llvmir2hll}"
cd "$OBJ"
for gcda in optimizer/optimizers/*.cpp.gcda; do
    src="$SRCDIR/${gcda%.gcda}"
    [ -f "$src" ] || continue
    gcov_out=$(gcov -b "$gcda" 2>&1)
    pct=$(echo "$gcov_out" | grep "File '$src'" -A3 | grep 'Lines executed' | head -1 | grep -oP '[0-9]+\.[0-9]+(?=%)')
    echo "${gcda##*/}: ${pct}%"
done | sort -t: -k2 -n
