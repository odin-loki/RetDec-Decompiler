#!/usr/bin/env bash
# Full decompilation coverage run (optional maintainer script):
# 1. Compile a diverse corpus of C programs at O0 and O2
# 2. Run the coverage-instrumented decompiler on each
# 3. Run the llvmir2hll unit tests
# 4. Generate an HTML report with gcovr/lcov
#
# Defaults assume a gcov-instrumented tree under build/linux. Override with env:
#   BUILD, SRCDIR, DECOMPILER, DECOMPILER_DATA, TESTS, CORPUS, REPORT_DIR
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

SRCDIR="${SRCDIR:-$REPO_ROOT}"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
DECOMPILER="${DECOMPILER:-$BUILD/src/retdec-decompiler/retdec-decompiler}"
DECOMPILER_DATA="${DECOMPILER_DATA:-$REPO_ROOT/src/retdec-decompiler}"
TESTS="${TESTS:-$BUILD/tests/llvmir2hll/retdec-tests-llvmir2hll}"

CORPUS="${CORPUS:-/tmp/coverage_corpus}"
REPORT_DIR="${REPORT_DIR:-/tmp/coverage_report}"
mkdir -p "$CORPUS" "$REPORT_DIR"

# ─── 1. Compile the test corpus ──────────────────────────────────────────────
echo "=== Compiling test corpus ==="

BIN_SRCS="$SRCDIR/tests/test_binaries"
# Also include the original fib/sort/linked_list from /tmp/ftest
OLD_SRCS=/tmp/ftest

PROGRAMS=(
    factorial switch_demo matrix string_ops binary_search
    bitops float_math stack_machine gcd_lcm hash_table
)

for prog in "${PROGRAMS[@]}"; do
    src="$BIN_SRCS/${prog}.c"
    if [ -f "$src" ]; then
        echo "  Compiling $prog..."
        # O0 version
        gcc -O0 -m64 -o "$CORPUS/${prog}_O0" "$src" -lm 2>/dev/null || echo "    WARNING: $prog O0 failed"
        # O2 version
        gcc -O2 -m64 -o "$CORPUS/${prog}_O2" "$src" -lm 2>/dev/null || echo "    WARNING: $prog O2 failed"
    fi
done

# Include existing fib/sort/ll binaries
for bin in fib fib_O2 sort ll; do
    src_bin="$OLD_SRCS/$bin"
    if [ -f "$src_bin" ]; then
        cp "$src_bin" "$CORPUS/${bin}"
        echo "  Copied $bin"
    fi
done

echo "  Corpus: $(ls $CORPUS | wc -l) binaries"

# ─── 2. Wipe existing coverage data so we start fresh ────────────────────────
echo ""
echo "=== Clearing old .gcda files ==="
find "$BUILD/src/llvmir2hll" -name '*.gcda' -delete 2>/dev/null
find "$BUILD/tests/llvmir2hll" -name '*.gcda' -delete 2>/dev/null
echo "  Done"

# ─── 3. Run the unit test suite ──────────────────────────────────────────────
echo ""
echo "=== Running llvmir2hll unit tests (coverage) ==="
"$TESTS" --gtest_color=no 2>&1 | tail -5

# ─── 4. Decompile every binary ───────────────────────────────────────────────
echo ""
echo "=== Decompiling corpus ==="
ok=0; fail=0
for bin in "$CORPUS"/*; do
    name=$(basename "$bin")
    out="$CORPUS/${name}.c"
    if "$DECOMPILER" -a x86-64 -f elf -o "$out" "$bin" \
            --config "$DECOMPILER_DATA/decompiler-config.json" \
            2>/dev/null; then
        ok=$((ok+1))
    else
        fail=$((fail+1))
        echo "  FAIL: $name"
    fi
done
echo "  OK=$ok  FAIL=$fail"

# ─── 5. Generate coverage report ─────────────────────────────────────────────
echo ""
echo "=== Generating coverage report ==="

LLVMIR2HLL_SRC="$SRCDIR/src/llvmir2hll"
LLVMIR2HLL_BUILD="$BUILD/src/llvmir2hll"
TESTS_BUILD="$BUILD/tests/llvmir2hll"

GCOVR="python3 -m gcovr"
if $GCOVR --version >/dev/null 2>&1; then
    echo "  Using gcovr (python3 -m gcovr)..."
    $GCOVR \
        --root "$SRCDIR" \
        --filter "$LLVMIR2HLL_SRC" \
        --object-directory "$LLVMIR2HLL_BUILD" \
        --object-directory "$TESTS_BUILD" \
        --html-details "$REPORT_DIR/index.html" \
        --html-title "RetDec llvmir2hll Coverage" \
        --print-summary \
        --txt "$REPORT_DIR/summary.txt" \
        --branch-coverage \
        2>&1
elif command -v lcov >/dev/null 2>&1; then
    echo "  Using lcov..."
    lcov \
        --capture \
        --directory "$LLVMIR2HLL_BUILD" \
        --directory "$TESTS_BUILD" \
        --base-directory "$SRCDIR" \
        --output-file "$REPORT_DIR/coverage.info" \
        --rc branch_coverage=1 \
        2>&1 | grep -v 'geninfo'
    lcov --extract "$REPORT_DIR/coverage.info" \
        "${LLVMIR2HLL_SRC}/*" \
        --output-file "$REPORT_DIR/coverage_filtered.info" 2>&1
    genhtml "$REPORT_DIR/coverage_filtered.info" \
        --output-directory "$REPORT_DIR" \
        --branch-coverage \
        --title "RetDec llvmir2hll Coverage" \
        --legend 2>&1 | tail -10
else
    echo "  Neither gcovr nor lcov found — generating raw gcov text output..."
    cd "$LLVMIR2HLL_BUILD"
    # Generate .gcov files for all source files
    find . -name '*.gcda' | while read gcda; do
        gcov -b -c -l "$gcda" 2>/dev/null
    done
    # Produce a summary
    echo "" > "$REPORT_DIR/summary.txt"
    echo "=== llvmir2hll Coverage Summary ===" >> "$REPORT_DIR/summary.txt"
    echo "" >> "$REPORT_DIR/summary.txt"
    total_lines=0; covered_lines=0
    for gcov_file in *.gcov **/*.gcov 2>/dev/null; do
        [ -f "$gcov_file" ] || continue
        lines=$(grep -c '^[[:space:]]*[0-9]' "$gcov_file" 2>/dev/null || echo 0)
        covered=$(grep -v '^[[:space:]]*#####' "$gcov_file" | grep -c '^[[:space:]]*[0-9]' 2>/dev/null || echo 0)
        total_lines=$((total_lines + lines))
        covered_lines=$((covered_lines + covered))
    done
    if [ $total_lines -gt 0 ]; then
        pct=$(echo "scale=1; $covered_lines * 100 / $total_lines" | bc)
        echo "Lines covered: $covered_lines / $total_lines  ($pct%)" >> "$REPORT_DIR/summary.txt"
    fi
    cat "$REPORT_DIR/summary.txt"
fi

echo ""
echo "=== Coverage run complete ==="
echo "Report written to: $REPORT_DIR"
ls -la "$REPORT_DIR/"
