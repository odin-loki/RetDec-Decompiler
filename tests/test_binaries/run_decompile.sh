#!/bin/bash
# =============================================================================
# run_decompile.sh — compile test binaries and decompile them with retdec
#
# Usage (from WSL, repo root or any cwd):
#   bash tests/test_binaries/run_decompile.sh
#
# Prerequisites:
#   - retdec built (default: <repo>/build/linux — override DECOMPILER=...)
#   - gcc available in WSL
# =============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DECOMPILER="${DECOMPILER:-$REPO_ROOT/build/linux/src/retdec-decompiler/retdec-decompiler}"
SRCDIR="$SCRIPT_DIR"
OUT=/tmp/rtest_out

mkdir -p "$OUT"

# Colour helpers
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; RESET='\033[0m'
ok()   { echo -e "${GREEN}[OK]${RESET}  $*"; }
fail() { echo -e "${RED}[FAIL]${RESET} $*"; }
info() { echo -e "${CYAN}====${RESET} $*"; }

# Verify the decompiler binary exists
if [ ! -x "$DECOMPILER" ]; then
    fail "Decompiler not found at $DECOMPILER — run wsl_build.sh first"
    exit 1
fi

# ---------------------------------------------------------------------------
# Compile helper: compile_bin <source.c> <output_name> <gcc_flags>
# ---------------------------------------------------------------------------
compile_bin() {
    local src="$1"
    local out_name="$2"
    shift 2
    local flags="$@"
    if gcc $flags -o "$OUT/$out_name" "$SRCDIR/$src" 2>/dev/null; then
        strip "$OUT/$out_name"
        ok "Compiled $src → $out_name ($flags)"
    else
        fail "Compile failed: $src"
    fi
}

# ---------------------------------------------------------------------------
# Decompile helper: decompile_bin <binary_name> <output_label>
# ---------------------------------------------------------------------------
decompile_bin() {
    local bin_name="$1"
    local label="$2"
    local bin_path="$OUT/$bin_name"
    local out_c="$OUT/${bin_name}.c"

    if [ ! -f "$bin_path" ]; then
        fail "Binary not found: $bin_path (skipping)"
        return
    fi

    info "Decompiling: $label"
    "$DECOMPILER" "$bin_path" -s -o "$out_c" 2>&1 \
        | grep -E "^(Phase|Error|Warning)" | grep -v "^Phase.*Running" || true

    if [ -f "$out_c" ]; then
        ok "Output: $out_c"
        echo ""
        echo "--- $label : BEGIN ---"
        cat "$out_c"
        echo "--- $label : END ---"
        echo ""
    else
        fail "No output generated for $label"
    fi
}

# ===========================================================================
# Phase 1: Compile all test binaries
# ===========================================================================
info "Compiling test binaries"

# Primary comprehensive coverage binary (two optimization levels)
compile_bin full_coverage.c   full_cov_O1  -O1
compile_bin full_coverage.c   full_cov_O2  -O2

# Existing focused test files
compile_bin fib.c             fib_O1       -O1
compile_bin fib.c             fib_O2       -O2
compile_bin sort.c            sort_O1      -O1
compile_bin linked_list.c     ll_O1        -O1
compile_bin switch_demo.c     switch_O1    -O1
compile_bin bitops.c          bitops_O1    -O1
compile_bin mba_patterns.c    mba_O1       -O1
compile_bin complex_loops.c   loops_O1     -O1
compile_bin ternary_and_casts.c ternary_O1 -O1
compile_bin char_arrays.c     chararr_O1   -O1
compile_bin type_conversions.c typeconv_O1 -O1
compile_bin recursion_heavy.c  recurse_O1  -O1

# ===========================================================================
# Phase 2: Decompile the comprehensive coverage binary first
# ===========================================================================
echo ""
info "=== PRIMARY: full_coverage.c at -O1 ==="
decompile_bin full_cov_O1 "full_coverage (-O1)"

echo ""
info "=== PRIMARY: full_coverage.c at -O2 ==="
decompile_bin full_cov_O2 "full_coverage (-O2)"

# ===========================================================================
# Phase 3: Decompile focused test binaries
# ===========================================================================

echo ""
info "=== FOCUSED TESTS ==="
decompile_bin sort_O1       "sort (-O1)"
decompile_bin ll_O1         "linked_list (-O1)"
decompile_bin fib_O2        "fibonacci (-O2)"
decompile_bin switch_O1     "switch_demo (-O1)"
decompile_bin bitops_O1     "bitops (-O1)"
decompile_bin mba_O1        "mba_patterns (-O1)"
decompile_bin loops_O1      "complex_loops (-O1)"
decompile_bin ternary_O1    "ternary_and_casts (-O1)"
decompile_bin chararr_O1    "char_arrays (-O1)"
decompile_bin typeconv_O1   "type_conversions (-O1)"
decompile_bin recurse_O1    "recursion_heavy (-O1)"

# ===========================================================================
# Phase 4: Summary
# ===========================================================================
echo ""
info "All decompiled outputs are in $OUT/"
echo "Run analyze_coverage.sh to check optimizer pattern coverage:"
echo "  bash $SRCDIR/analyze_coverage.sh"
