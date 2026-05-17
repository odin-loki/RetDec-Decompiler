#!/usr/bin/env bash
# Full coverage collection: unit tests + 54 binaries + special decompiler modes
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build/linux}"
DECOMPILER="${DECOMPILER:-$BUILD/src/retdec-decompiler/retdec-decompiler}"
CONFIG="${CONFIG:-$REPO_ROOT/src/retdec-decompiler/decompiler-config.json}"
TESTS=$BUILD/tests/llvmir2hll/retdec-tests-llvmir2hll
CORPUS=/tmp/coverage_corpus

echo "=== Clearing stale .gcda files ==="
find $BUILD/src/llvmir2hll -name '*.gcda' -delete 2>/dev/null || true
find $BUILD/tests/llvmir2hll -name '*.gcda' -delete 2>/dev/null || true
echo "  Done"

echo ""
echo "=== Running llvmir2hll unit tests ==="
"$TESTS" --gtest_color=no 2>&1 | grep -E '\[==========\].*ran|PASSED|FAILED' | tail -3
echo "  Unit tests done"

echo ""
echo "=== Decompiling corpus (standard mode) ==="
ok=0; fail=0
for bin in $CORPUS/*; do
    case "$bin" in *.c|*.json|*.ll|*.bc|*.dsm|*_decomp*) continue ;; esac
    [ -f "$bin" ] && [ -x "$bin" ] || continue
    name=$(basename "$bin")
    out="${bin}_decomp.c"
    if "$DECOMPILER" --output "$out" --config "$CONFIG" "$bin" > /dev/null 2>&1; then
        ok=$((ok+1))
    else
        echo "  FAIL: $name"
        fail=$((fail+1))
    fi
done
echo "  Standard: OK=$ok FAIL=$fail"

echo ""
echo "=== Decompiling with --backend-emit-cfg (covers CFG/BIR writers) ==="
cfg_ok=0
for bin in $CORPUS/factorial_O0 $CORPUS/binary_search_O0 $CORPUS/gcd_lcm_O0 \
           $CORPUS/recursion_heavy_O0 $CORPUS/complex_loops_O0 \
           $CORPUS/complex_control_flow_O0 $CORPUS/switch_demo_O0; do
    [ -f "$bin" ] && [ -x "$bin" ] || continue
    name=$(basename "$bin")
    out="/tmp/cfg_${name}_decomp.c"
    if "$DECOMPILER" --output "$out" --config "$CONFIG" \
            --backend-emit-cfg --backend-emit-cg "$bin" > /dev/null 2>&1; then
        cfg_ok=$((cfg_ok+1))
    fi
done
echo "  CFG/CG emit: OK=$cfg_ok"

echo ""
echo "=== Decompiling with varied options (covers optimizer branches) ==="
VAR_BIN=$CORPUS/hash_table_O0
# With pessimistic call info (covers pessim_call_info_obtainer)
"$DECOMPILER" --output /tmp/pessim_out.c --config "$CONFIG" \
    --backend-call-info-obtainer pessim "$VAR_BIN" > /dev/null 2>&1 && echo "  pessim-obtainer: OK"
# With address-based var renaming
"$DECOMPILER" --output /tmp/addr_out.c --config "$CONFIG" \
    --backend-var-renamer address "$VAR_BIN" > /dev/null 2>&1 && echo "  address-renamer: OK"
# With hungarian naming
"$DECOMPILER" --output /tmp/hung_out.c --config "$CONFIG" \
    --backend-var-renamer hungarian "$VAR_BIN" > /dev/null 2>&1 && echo "  hungarian-renamer: OK"
# With simple naming
"$DECOMPILER" --output /tmp/simple_out.c --config "$CONFIG" \
    --backend-var-renamer simple "$VAR_BIN" > /dev/null 2>&1 && echo "  simple-renamer: OK"
# Without optimizations (covers different branches)
"$DECOMPILER" --output /tmp/noopt_out.c --config "$CONFIG" \
    --backend-no-opts "$VAR_BIN" > /dev/null 2>&1 && echo "  no-opts: OK"
# Keeping all brackets
"$DECOMPILER" --output /tmp/brackets_out.c --config "$CONFIG" \
    --backend-keep-all-brackets "$VAR_BIN" > /dev/null 2>&1 && echo "  keep-brackets: OK"
# Keeping library funcs
"$DECOMPILER" --output /tmp/libfuncs_out.c --config "$CONFIG" \
    --backend-keep-library-funcs "$VAR_BIN" > /dev/null 2>&1 && echo "  keep-libfuncs: OK"
# Selective function decompilation
"$DECOMPILER" --output /tmp/select_out.c --config "$CONFIG" \
    --select-functions main "$VAR_BIN" > /dev/null 2>&1 && echo "  select-funcs: OK"

echo ""
echo "=== Additional binaries with CFG emit ==="
for bin in $CORPUS/dynamic_alloc_O0 $CORPUS/varargs_and_pointers_O0 \
           $CORPUS/globals_and_structs_O2 $CORPUS/mba_patterns_O0; do
    name=$(basename "$bin")
    out="/tmp/cfg2_${name}.c"
    "$DECOMPILER" --output "$out" --config "$CONFIG" --backend-emit-cfg "$bin" > /dev/null 2>&1 \
        && echo -n "  $name:OK " || echo -n "  $name:FAIL "
done
echo ""

echo ""
echo "=== Coverage data collected ==="
gcda_count=$(find $BUILD/src/llvmir2hll -name '*.gcda' | wc -l)
echo "  .gcda files: $gcda_count"

# Spot-check key files
cd $BUILD/src/llvmir2hll/CMakeFiles/llvmir2hll.dir
echo "  Key files:"
for f in llvmir2hll.cpp \
          optimizer/optimizer_manager.cpp \
          hll/hll_writers/c_hll_writer.cpp \
          hll/bir_writer.cpp \
          graphs/cfg/cfg_writer.cpp \
          optimizer/optimizers/while_true_to_for_loop_optimizer.cpp; do
    gcda="${f%.cpp}.cpp.gcda"
    if [ -f "$gcda" ]; then
        pct=$(gcov -b "$gcda" 2>&1 | grep "^File.*${f##*/}" -A1 | grep 'Lines executed' | grep -oE '[0-9]+\.[0-9]+' | head -1)
        echo "    $f: ${pct:-0}%"
    fi
done
