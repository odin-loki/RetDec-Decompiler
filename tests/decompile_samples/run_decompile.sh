#!/usr/bin/env bash
# Run retdec-decompiler on every compiled sample and report quality metrics.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DECOMPILER="${DECOMPILER:-$REPO_ROOT/build/linux/src/retdec-decompiler/retdec-decompiler}"
TESTDIR=/tmp/retdec_tests
OUTDIR=/tmp/retdec_out
mkdir -p "$OUTDIR"

OK=0; FAIL=0; WARN=0
pass(){ echo "[ok]   $1"; OK=$(( OK + 1 )); }
warn(){ echo "[warn] $1: $2"; WARN=$(( WARN + 1 )); }
fail(){ echo "[FAIL] $1: $2"; FAIL=$(( FAIL + 1 )); }

run_decomp(){
    local label="$1"
    local input="$2"
    local extra="${3:-}"
    local out="$OUTDIR/$(basename "$input").c"

    if [ ! -f "$input" ]; then
        fail "$label" "input file missing: $input"
        return
    fi

    # Run decompiler (60s timeout)
    timeout 120 "$DECOMPILER" "$input" -o "$out" $extra -s 2>&1
    local rc=$?

    if [ $rc -eq 124 ]; then
        warn "$label" "timed out (120s)"
        return
    fi
    if [ $rc -ne 0 ]; then
        fail "$label" "exit code $rc"
        return
    fi
    if [ ! -f "$out" ]; then
        fail "$label" "no output file produced"
        return
    fi

    local lines=$(wc -l < "$out")
    local funcs=$(grep -c 'int32_t\|void \|int \|char \|float \|double \|uint' "$out" 2>/dev/null || echo 0)
    echo "       -> $lines lines, ~$funcs typed declarations"
    pass "$label"
}

echo "============================================================"
echo " RetDec Decompilation Test Suite"
echo "============================================================"
echo ""

# ──── NATIVE / COMPILED BINARIES ────────────────────────────────

echo "--- Native ELF binaries ---"
run_decomp "C   (sort)"      "$TESTDIR/c/sort"
run_decomp "C++ (graph BFS)" "$TESTDIR/cpp/graph"
run_decomp "Rust (primes)"   "$TESTDIR/rust/primes"
run_decomp "Go   (matrix)"   "$TESTDIR/go/matrix"
run_decomp "x86-ASM (strlen)" "$TESTDIR/asm/strlen"

echo ""
echo "--- Managed / bytecode formats ---"

# Java .class (RetDec reads .class natively via its JVM lifter)
run_decomp "Java .class (Fib)" "$TESTDIR/java/Fib.class"

# DEX / Android
run_decomp "DEX (classes.dex)" "$TESTDIR/dex/classes.dex"

# Python .pyc
run_decomp "Python .pyc" "$TESTDIR/python/primes.pyc"

# Lua bytecode
run_decomp "Lua bytecode" "$TESTDIR/lua/mergesort.luac"

# WebAssembly .wasm
run_decomp "WebAssembly .wasm" "$TESTDIR/wasm/math.wasm"

# Kotlin .jar (JVM bytecode jar)
run_decomp "Kotlin .jar" "$TESTDIR/kotlin/Hello.jar"

# C# .dll (CIL bytecode)
run_decomp "C# .dll (BubbleSort)" "$TESTDIR/csharp/publish/BubbleSort.dll"

# ──────────────────────────────────────────────────────────────────

echo ""
echo "============================================================"
echo " Results: $OK passed  |  $WARN warnings  |  $FAIL failed"
echo "============================================================"
echo ""

# Show snippet of each successful output for quality review
for f in "$OUTDIR"/*.c; do
    [ -f "$f" ] || continue
    local_name=$(basename "$f")
    echo "── $local_name (first 20 meaningful lines) ──"
    grep -v '^$\|^//' "$f" | head -20
    echo ""
done
