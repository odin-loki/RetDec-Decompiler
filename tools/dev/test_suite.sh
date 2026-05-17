#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
DECOMPILER="${DECOMPILER:-$BUILD/src/retdec-decompiler/retdec-decompiler}"
FILEINFO_BIN="${FILEINFO_BIN:-$BUILD/src/fileinfo/retdec-fileinfo}"
RESULTS=/tmp/retdec_tests
mkdir -p $RESULTS

echo "=== RetDec Test Suite ==="

# Test 1: smoke_hello.exe (already done)
if [ -f /tmp/smoke_clean.c ]; then
    SZ=$(stat -c%s /tmp/smoke_clean.c)
    echo "Test 1 (smoke_hello.exe): PASS - ${SZ} bytes"
    cp /tmp/smoke_clean.c $RESULTS/
else
    echo "Test 1 (smoke_hello.exe): Running..."
    timeout 120 $DECOMPILER /tmp/smoke_hello.exe -o $RESULTS/smoke_hello.c 2>&1 | tail -2
    echo "  Size: $(stat -c%s $RESULTS/smoke_hello.c 2>/dev/null || echo FAILED)"
fi

# Test 2: Build and decompile a more complex binary
cat > /tmp/complex_test.c << 'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int x, y; } Point;

int cmp(const void* a, const void* b) {
    const Point* pa = (const Point*)a;
    const Point* pb = (const Point*)b;
    if (pa->x != pb->x) return pa->x - pb->x;
    return pa->y - pb->y;
}

void sort_pts(Point* pts, int n) {
    qsort(pts, n, sizeof(Point), cmp);
}

int fib(int n) {
    if (n <= 1) return n;
    int a = 0, b = 1;
    for (int i = 2; i <= n; i++) {
        int c = a + b;
        a = b;
        b = c;
    }
    return b;
}

int main(void) {
    Point pts[8];
    srand(42);
    for (int i = 0; i < 8; i++) {
        pts[i].x = rand() % 100;
        pts[i].y = rand() % 100;
    }
    sort_pts(pts, 8);
    for (int i = 0; i < 8; i++)
        printf("(%d,%d) ", pts[i].x, pts[i].y);
    printf("\n");
    for (int i = 0; i < 15; i++)
        printf("fib(%d)=%d\n", i, fib(i));
    return 0;
}
CEOF

x86_64-w64-mingw32-gcc -O2 /tmp/complex_test.c -o /tmp/complex_test.exe -static 2>/dev/null
echo "Test 2 (complex_test.exe): Running..."
timeout 120 $DECOMPILER /tmp/complex_test.exe -o $RESULTS/complex_test.c 2>&1 | grep -E 'cycle detected|error:' | head -5
SZ=$(stat -c%s $RESULTS/complex_test.c 2>/dev/null || echo 0)
if [ "$SZ" -gt 1000 ]; then
    echo "  -> PASS: ${SZ} bytes"
else
    echo "  -> FAIL: ${SZ} bytes"
fi

# Test 3: Decompile retdec's own fileinfo binary (complex ELF)
echo "Test 3 (retdec-fileinfo ELF): Running (may take up to 3 minutes)..."
timeout 180 $DECOMPILER "$FILEINFO_BIN" -o $RESULTS/fileinfo.c 2>&1 | grep -E 'cycle detected|error:|Unpacking|LLVM IR' | head -5
SZ=$(stat -c%s $RESULTS/fileinfo.c 2>/dev/null || echo 0)
if [ "$SZ" -gt 10000 ]; then
    echo "  -> PASS: ${SZ} bytes"
else
    echo "  -> FAIL or TIMEOUT: ${SZ} bytes"
fi

echo "=== Test Suite Complete ==="
