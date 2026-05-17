#!/usr/bin/env bash
mkdir -p /tmp/coverage_corpus
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SRC="${SRC:-$REPO_ROOT/tests/test_binaries}"

ALL_PROGS=(
    factorial switch_demo matrix string_ops binary_search
    bitops float_math stack_machine gcd_lcm hash_table
    char_arrays complex_loops recursion_heavy globals_and_structs
    memcpy_intrinsics ternary_and_casts complex_control_flow
    varargs_and_pointers dynamic_alloc exception_like global_init
    mba_patterns type_conversions simd_like error_handling
)

ok=0; fail=0
for prog in "${ALL_PROGS[@]}"; do
    src="$SRC/${prog}.c"
    if [ ! -f "$src" ]; then
        echo "  MISSING: $src"
        continue
    fi
    echo -n "  $prog: "
    if gcc -O0 -m64 -o /tmp/coverage_corpus/${prog}_O0 "$src" -lm 2>/dev/null; then
        echo -n "O0 "; ok=$((ok+1))
    else
        echo -n "O0-FAIL "; fail=$((fail+1))
        # Try with relaxed settings
        gcc -O0 -m64 -o /tmp/coverage_corpus/${prog}_O0 "$src" -lm -lgcc 2>/dev/null && echo -n "O0(retry) " && ok=$((ok+1))
    fi
    if gcc -O2 -m64 -o /tmp/coverage_corpus/${prog}_O2 "$src" -lm 2>/dev/null; then
        echo -n "O2 "; ok=$((ok+1))
    else
        echo -n "O2-FAIL "; fail=$((fail+1))
    fi
    echo
done

# Copy old binaries
for bin in fib fib_O2 sort ll; do
    [ -f /tmp/ftest/$bin ] && cp /tmp/ftest/$bin /tmp/coverage_corpus/ && echo "  copied $bin"
done

echo ""
echo "=== Corpus: $(ls /tmp/coverage_corpus/ | grep -v '\.' | wc -l) binaries  (ok=$ok fail=$fail) ==="
ls /tmp/coverage_corpus/ | grep -v '\.'
