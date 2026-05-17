#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
SRCDIR="${SRCDIR:-$REPO_ROOT/tests/test_binaries}"
OUT=/tmp/ftest
DECOMPILER="${DECOMPILER:-$BUILD/src/retdec-decompiler/retdec-decompiler}"

mkdir -p "$OUT"

gcc -O1 -o "$OUT/fib" "$SRCDIR/fib.c" && strip "$OUT/fib" && echo "fib compiled"
gcc -O2 -o "$OUT/fib_O2" "$SRCDIR/fib.c" && strip "$OUT/fib_O2" && echo "fib_O2 compiled"
gcc -O1 -o "$OUT/sort" "$SRCDIR/sort.c" && strip "$OUT/sort" && echo "sort compiled"
gcc -O1 -o "$OUT/ll" "$SRCDIR/linked_list.c" && strip "$OUT/ll" && echo "ll compiled"

echo "=== Decompiling ==="
"$DECOMPILER" "$OUT/fib" -s -o "$OUT/fib.c" 2>/dev/null && echo "fib decompiled"
"$DECOMPILER" "$OUT/fib_O2" -s -o "$OUT/fib_O2.c" 2>/dev/null && echo "fib_O2 decompiled"
"$DECOMPILER" "$OUT/sort" -s -o "$OUT/sort.c" 2>/dev/null && echo "sort decompiled"
"$DECOMPILER" "$OUT/ll" -s -o "$OUT/ll.c" 2>/dev/null && echo "ll decompiled"

echo ""
echo "==================================="
echo "FIBONACCI O1 - FIRST KEY FUNCTION"
echo "==================================="
grep -A 20 'function_116' "$OUT/fib.c" | head -35

echo ""
echo "==================================="
echo "FIBONACCI O1 - SECOND KEY FUNCTION"
echo "==================================="
grep -A 25 'function_119' "$OUT/fib.c" | head -35

echo ""
echo "==================================="
echo "FIBONACCI O2"
echo "==================================="
cat "$OUT/fib_O2.c" | grep -v '^#\|^//' | head -80

echo ""
echo "==================================="
echo "SORT - KEY FUNCTION"
echo "==================================="
grep -A 30 'function_11[0-9a-f][0-9a-f]' "$OUT/sort.c" | head -60

echo ""
echo "==================================="
echo "LINKED LIST"
echo "==================================="
cat "$OUT/ll.c" | head -100
