#!/bin/bash
# Full test suite for retdec on Linux

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
DECOMPILER_DIR="${DECOMPILER_DIR:-$BUILD/src/retdec-decompiler}"
DECOMPILER=$DECOMPILER_DIR/retdec-decompiler
OUTDIR=/tmp/retdec_tests
PASS=0
FAIL=0

mkdir -p "$OUTDIR"

run_test() {
    local name="$1"
    local binary="$2"
    local out="$OUTDIR/$name.c"
    echo ""
    echo "=== TEST: $name ==="
    cd "$DECOMPILER_DIR"
    ./retdec-decompiler "$binary" -o "$out" 2>&1 | grep -E 'Running phase|Error|error|LLVM ERROR|exit|Exception|warning.*abort' | head -20
    local status=${PIPESTATUS[0]}
    if [ $status -eq 0 ] && [ -f "$out" ] && [ -s "$out" ]; then
        local size=$(wc -c < "$out")
        echo "PASS: $name ($size bytes)"
        PASS=$((PASS+1))
    else
        echo "FAIL: $name (exit=$status, file_exists=$(test -f "$out" && echo yes || echo no))"
        FAIL=$((FAIL+1))
    fi
}

# --- Test 1: Simple ELF (factorial + fib) ---
cat > /tmp/t1.c << 'EOF'
#include <stdio.h>
int factorial(int n) { return n <= 1 ? 1 : n * factorial(n-1); }
int fib(int n) { return n <= 1 ? n : fib(n-1) + fib(n-2); }
int main() { printf("%d %d\n", factorial(5), fib(10)); return 0; }
EOF
gcc -O1 -o /tmp/t1_elf /tmp/t1.c
run_test "simple_elf" /tmp/t1_elf

# --- Test 2: ELF with loops and arrays ---
cat > /tmp/t2.c << 'EOF'
#include <stdio.h>
int arr[10];
int sum_array(int *a, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return s;
}
void bubble_sort(int *a, int n) {
    for (int i = 0; i < n-1; i++)
        for (int j = 0; j < n-i-1; j++)
            if (a[j] > a[j+1]) { int t = a[j]; a[j] = a[j+1]; a[j+1] = t; }
}
int main() {
    for (int i = 0; i < 10; i++) arr[i] = 10 - i;
    bubble_sort(arr, 10);
    printf("%d\n", sum_array(arr, 10));
    return 0;
}
EOF
gcc -O1 -o /tmp/t2_elf /tmp/t2.c
run_test "loops_arrays_elf" /tmp/t2_elf

# --- Test 3: ELF with structs ---
cat > /tmp/t3.c << 'EOF'
#include <stdio.h>
#include <string.h>
typedef struct { char name[32]; int age; float score; } Person;
void print_person(Person *p) { printf("%s %d %.1f\n", p->name, p->age, p->score); }
int main() {
    Person p;
    strcpy(p.name, "Alice");
    p.age = 30;
    p.score = 95.5f;
    print_person(&p);
    return 0;
}
EOF
gcc -O1 -o /tmp/t3_elf /tmp/t3.c
run_test "structs_elf" /tmp/t3_elf

# --- Test 4: ELF with -O2 optimization ---
cat > /tmp/t4.c << 'EOF'
#include <stdio.h>
int gcd(int a, int b) { return b == 0 ? a : gcd(b, a % b); }
int lcm(int a, int b) { return a / gcd(a, b) * b; }
int main() { printf("%d %d\n", gcd(48, 18), lcm(12, 18)); return 0; }
EOF
gcc -O2 -o /tmp/t4_elf /tmp/t4.c
run_test "optimized_elf" /tmp/t4_elf

# --- Test 5: Windows PE (cross-compiled) ---
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    cat > /tmp/t5.c << 'EOF'
#include <stdio.h>
int main() {
    int x = 42;
    printf("hello %d\n", x);
    return 0;
}
EOF
    x86_64-w64-mingw32-gcc -O1 -o /tmp/t5.exe /tmp/t5.c 2>/dev/null || true
    if [ -f /tmp/t5.exe ]; then
        run_test "pe_exe" /tmp/t5.exe
    else
        echo "=== SKIP: Windows PE (no cross compiler) ==="
    fi
fi

# --- Test 6: smoke_hello.exe if it exists ---
SMOKE=/tmp/smoke_hello.exe
if [ -f "$SMOKE" ]; then
    run_test "smoke_hello_exe" "$SMOKE"
fi

echo ""
echo "============================================"
echo "RESULTS: $PASS passed, $FAIL failed"
echo "============================================"
