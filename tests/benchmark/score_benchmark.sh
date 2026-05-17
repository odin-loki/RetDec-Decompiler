#!/usr/bin/env bash
# score_benchmark.sh — Analyse decompiled output and produce a JSON score card.
#
# Usage: bash score_benchmark.sh [out-dir] [result-json]
#   out-dir     defaults to /tmp/benchmark_out
#   result-json defaults to $out-dir/score.json
#
# Scoring rubric (max 100 pts, 5 pts each, 20 checks):
#
#  01 FuncNameRecovery    Key function names preserved in output
#  02 LoopRecovery        While/for loops reconstructed (not just goto spaghetti)
#  03 BitShiftOptimizer   x*8 → x<<3, x/4 → x>>2
#  04 MBASimplify         (a^b)+2*(a&b) fully simplified to a+b
#  05 SelfAssignRemoval   No x = x; assignments remain
#  06 ArithIdentities     No +0, *1, -0 identity expressions remain
#  07 GotoCFGOptimizer    Minimal/zero goto statements
#  08 CastRemoval         Cast density below threshold
#  09 GuardClausePattern  Early-return "if (x==0) return -1;" pattern
#  10 NoInitVarDef        Minimal bare "int x;" declarations
#  11 TypeInference       Concrete integer types (int32_t, uint32_t, uint8_t…)
#  12 RecursionQuality    fibonacci / factorial / gcd call themselves correctly
#  13 FuncPtrDispatch     dispatch / apply_op / indirect call patterns present
#  14 StringLiterals      String constants recovered from data section
#  15 BinarySearchQuality Binary search structure recognizable
#  16 BubbleSortQuality   Bubble sort / nested swap loops recognizable
#  17 PtrWalkPattern      sum_ptr_walk pointer-increment pattern
#  18 StructFieldAccess   Struct field reads via pointer offset (linked list)
#  19 MemoryOps           malloc / free calls correctly preserved
#  20 BitOperations       Bitwise pack_rgba / extract_channel / toggle_bits

set -uo pipefail

OUT_DIR="${1:-/tmp/benchmark_out}"
RESULT_JSON="${2:-$OUT_DIR/score.json}"
DECOMPILED="$OUT_DIR/benchmark_O1.c"         # C benchmark primary target
DECOMPILED_CPP="$OUT_DIR/benchmark_cpp_O1.c" # C++ benchmark primary target

if [ ! -f "$DECOMPILED" ]; then
    echo "ERROR: Decompiled C file not found: $DECOMPILED" >&2
    echo "Run run_benchmark.sh first." >&2
    exit 1
fi

# ─── helpers ──────────────────────────────────────────────────────────────────
total_score=0
max_score=200    # 100 pts C + 100 pts C++
declare -a check_names=()
declare -a check_scores=()
declare -a check_max=()
declare -a check_status=()
declare -a check_notes=()

cnt() {
    # cnt <file> <pattern>  — returns grep -cE count, never fails
    local n
    n=$(grep -cE "$2" "$1" 2>/dev/null) || n=0
    printf '%s' "${n:-0}"
}

add_check() {
    local name="$1" pts="$2" max_pts="$3" status="$4" note="$5"
    check_names+=("$name")
    check_scores+=("$pts")
    check_max+=("$max_pts")
    check_status+=("$status")
    check_notes+=("$note")
    total_score=$(( total_score + pts ))
}

# ─── Check 01: Function Name Recovery ─────────────────────────────────────────
# Expect all named functions to appear in output (unstripped binary)
key_funcs=("day_name" "fibonacci" "factorial" "gcd" "binary_search"
           "bubble_sort" "list_sum" "list_count" "safe_divide"
           "mba_add" "dispatch" "alloc_and_fill")
found_funcs=0
for fn in "${key_funcs[@]}"; do
    grep -qE "\b${fn}\b" "$DECOMPILED" && found_funcs=$(( found_funcs + 1 ))
done
n_key=${#key_funcs[@]}
if   [ "$found_funcs" -ge "$n_key"      ]; then add_check "FuncNameRecovery"  5 5 "PASS"    "${found_funcs}/${n_key} names"
elif [ "$found_funcs" -ge $(( n_key*3/4 )) ]; then add_check "FuncNameRecovery" 3 5 "PARTIAL" "${found_funcs}/${n_key} names"
else                                              add_check "FuncNameRecovery"  0 5 "FAIL"    "${found_funcs}/${n_key} names"
fi

# ─── Check 02: Loop Recovery ──────────────────────────────────────────────────
# Loops should be while/for, not raw goto spaghetti
whl=$(cnt "$DECOMPILED" '\bwhile\b')
fl=$(cnt  "$DECOMPILED" '\bfor\b')
loops=$(( whl + fl ))
if   [ "$loops" -ge 12 ]; then add_check "LoopRecovery"  5 5 "PASS"    "${whl} while, ${fl} for"
elif [ "$loops" -ge  6 ]; then add_check "LoopRecovery"  3 5 "PARTIAL" "${whl} while, ${fl} for"
else                            add_check "LoopRecovery"  0 5 "FAIL"    "${whl} while, ${fl} for"
fi

# ─── Check 03: BitShiftOptimizer ──────────────────────────────────────────────
# shift_mul should contain << 3, shift_div should contain >> 2 (or >>)
shl=$(cnt "$DECOMPILED" '\bshift_mul\b[^{]*\{.*<<\s*3')   # too narrow; use simpler
# Look inside the shift_mul function body
shift_mul_body=$(sed -n '/recovered range: shift_mul/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
shift_div_body=$(sed -n '/recovered range: shift_div/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
has_shl=0; has_shr=0
echo "$shift_mul_body" | grep -qE '<<\s*3' && has_shl=1
# BitShiftOptimizer converts signed-divide shift patterns → /N (division form)
# So accept either >> or /[2468] as evidence the optimizer fired for shift_div
echo "$shift_div_body" | grep -qE '>>|/\s*[2468]\b' && has_shr=1
if   [ "$has_shl" -eq 1 ] && [ "$has_shr" -eq 1 ]; then add_check "BitShiftOptimizer" 5 5 "PASS"    "shift_mul<<3 and shift_div/N recovered"
elif [ "$has_shl" -eq 1 ] || [ "$has_shr" -eq 1 ]; then add_check "BitShiftOptimizer" 3 5 "PARTIAL" "only one shift pattern recovered"
else                                                      add_check "BitShiftOptimizer" 0 5 "FAIL"    "no shift recovery"
fi

# ─── Check 04: MBA Simplification ─────────────────────────────────────────────
# mba_add: (a^b)+2*(a&b) → just "return a2 + a1" or "return a1 + a2"
mba_add_body=$(sed -n '/recovered range: mba_add/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
mba_simple=0
# Simple: body contains only a return with + and no XOR/AND
echo "$mba_add_body" | grep -qE 'return\s+\(?a[12]\s*\+\s*a[12]\)?' && mba_simple=1
# Partial: body has fewer than 3 operators (partially simplified)
mba_ops=$(echo "$mba_add_body" | grep -cE '\^|&|\|' 2>/dev/null || echo 9)
if   [ "$mba_simple" -eq 1 ];           then add_check "MBASimplify" 5 5 "PASS"    "mba_add fully simplified"
elif [ "${mba_ops:-9}" -le 2 ];         then add_check "MBASimplify" 3 5 "PARTIAL" "${mba_ops} bitwise ops remain"
else                                         add_check "MBASimplify" 0 5 "FAIL"    "MBA not simplified (${mba_ops} ops)"
fi

# ─── Check 05: SelfAssignRemoval ──────────────────────────────────────────────
sa=$(cnt "$DECOMPILED" '\b(\w+)\s*=\s*\1\s*;')
if   [ "$sa" -eq 0 ]; then add_check "SelfAssignRemoval" 5 5 "PASS"    "zero self-assignments"
elif [ "$sa" -le 2 ]; then add_check "SelfAssignRemoval" 3 5 "PARTIAL" "${sa} self-assignment(s)"
else                        add_check "SelfAssignRemoval" 0 5 "FAIL"    "${sa} self-assignments"
fi

# ─── Check 06: Arithmetic Identities ──────────────────────────────────────────
ari=$(cnt "$DECOMPILED" '[^<>]\+\s*0[^x]|\*\s*1[^0-9]|[^<>]\-\s*0[^x]|\|\s*0[^x]')
if   [ "$ari" -eq 0 ]; then add_check "ArithIdentities" 5 5 "PASS"    "no identity ops"
elif [ "$ari" -le 2 ]; then add_check "ArithIdentities" 3 5 "PARTIAL" "${ari} identity ops"
else                         add_check "ArithIdentities" 0 5 "FAIL"    "${ari} identity ops remain"
fi

# ─── Check 07: GotoCFGOptimizer ───────────────────────────────────────────────
gotos=$(cnt "$DECOMPILED" '\bgoto\b')
if   [ "$gotos" -eq 0 ]; then add_check "GotoCFGOptimizer" 5 5 "PASS"    "zero goto(s)"
elif [ "$gotos" -le 3 ]; then add_check "GotoCFGOptimizer" 3 5 "PARTIAL" "${gotos} goto(s)"
else                           add_check "GotoCFGOptimizer" 0 5 "FAIL"    "${gotos} goto(s)"
fi

# ─── Check 08: Cast Removal ───────────────────────────────────────────────────
# Count explicit C-style integer casts vs total statements
casts=$(cnt "$DECOMPILED" '\(u?int[0-9]+_t\)')
stmts=$(cnt "$DECOMPILED" ';$')
stmts=${stmts:-1}   # avoid div/0
ratio=$(( casts * 100 / stmts ))
if   [ "$ratio" -le 40 ]; then add_check "CastRemoval" 5 5 "PASS"    "${casts} casts / ${stmts} stmts (${ratio}%)"
elif [ "$ratio" -le 70 ]; then add_check "CastRemoval" 3 5 "PARTIAL" "${casts} casts / ${stmts} stmts (${ratio}%)"
else                            add_check "CastRemoval" 0 5 "FAIL"    "${casts} casts / ${stmts} stmts (${ratio}%)"
fi

# ─── Check 09: Guard Clause Pattern ───────────────────────────────────────────
# Early-return guards: "if (...) { return ...; }" style
guards=$(cnt "$DECOMPILED" 'if\s*\([^)]+\)\s*\{?[[:space:]]*$')
# Also count "if (...)\n    return" pattern
guards2=$(cnt "$DECOMPILED" 'if\s*\(.*\)\s*\{?')
if   [ "$guards2" -ge 15 ]; then add_check "GuardClausePattern" 5 5 "PASS"    "${guards2} if-guards"
elif [ "$guards2" -ge  6 ]; then add_check "GuardClausePattern" 3 5 "PARTIAL" "${guards2} if-guards"
else                              add_check "GuardClausePattern" 0 5 "FAIL"    "${guards2} if-guards"
fi

# ─── Check 10: NoInitVarDef ───────────────────────────────────────────────────
# Count function-body bare decls with no initializer ("    int x;")
uninit=$(cnt "$DECOMPILED" '^\s+(int|int32_t|uint32_t|int64_t|uint64_t)\s+[a-zA-Z_]\w*\s*;')
if   [ "$uninit" -le 5 ]; then add_check "NoInitVarDef" 5 5 "PASS"    "${uninit} bare decl(s)"
elif [ "$uninit" -le 10 ]; then add_check "NoInitVarDef" 3 5 "PARTIAL" "${uninit} bare decl(s)"
else                             add_check "NoInitVarDef" 0 5 "FAIL"    "${uninit} bare decl(s)"
fi

# ─── Check 11: Type Inference ─────────────────────────────────────────────────
ctypes=$(cnt "$DECOMPILED" '\b(int32_t|uint32_t|uint8_t|int64_t|uint16_t|uint64_t)\b')
if   [ "$ctypes" -ge 30 ]; then add_check "TypeInference" 5 5 "PASS"    "${ctypes} concrete type(s)"
elif [ "$ctypes" -ge 10 ]; then add_check "TypeInference" 3 5 "PARTIAL" "${ctypes} concrete type(s)"
else                             add_check "TypeInference" 0 5 "FAIL"    "${ctypes} concrete type(s)"
fi

# ─── Check 12: Recursion Quality ──────────────────────────────────────────────
# fibonacci, factorial and gcd must call themselves
fib_self=0; fac_self=0; gcd_self=0
fib_body=$(sed -n '/recovered range: fibonacci/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
fac_body=$(sed -n '/recovered range: factorial/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
gcd_body=$(sed -n '/recovered range: gcd/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
echo "$fib_body" | grep -qE '\bfibonacci\s*\('  && fib_self=1
echo "$fac_body" | grep -qE '\bfactorial\s*\('  && fac_self=1
echo "$gcd_body" | grep -qE '\bgcd\s*\('         && gcd_self=1
rec_total=$(( fib_self + fac_self + gcd_self ))
if   [ "$rec_total" -eq 3 ]; then add_check "RecursionQuality" 5 5 "PASS"    "all 3 recursive"
elif [ "$rec_total" -ge 2 ]; then add_check "RecursionQuality" 3 5 "PARTIAL" "${rec_total}/3 recursive"
else                               add_check "RecursionQuality" 0 5 "FAIL"    "${rec_total}/3 recursive"
fi

# ─── Check 13: Function Pointer Dispatch ──────────────────────────────────────
fptr=$(cnt "$DECOMPILED" '\bdispatch\b|\bapply_op\b|\bBinOp\b')
if   [ "$fptr" -ge 3 ]; then add_check "FuncPtrDispatch" 5 5 "PASS"    "${fptr} fp pattern(s)"
elif [ "$fptr" -ge 1 ]; then add_check "FuncPtrDispatch" 3 5 "PARTIAL" "${fptr} fp pattern(s)"
else                          add_check "FuncPtrDispatch" 0 5 "FAIL"    "no function pointer dispatch"
fi

# ─── Check 14: String Literals ────────────────────────────────────────────────
strlits=$(cnt "$DECOMPILED" '"[A-Za-z][A-Za-z0-9 !?.,_:-]{2,}"')
if   [ "$strlits" -ge 4 ]; then add_check "StringLiterals" 5 5 "PASS"    "${strlits} string literal(s)"
elif [ "$strlits" -ge 1 ]; then add_check "StringLiterals" 3 5 "PARTIAL" "${strlits} string literal(s)"
else                             add_check "StringLiterals" 0 5 "FAIL"    "no string literals"
fi

# ─── Check 15: Binary Search Quality ──────────────────────────────────────────
bs_body=$(sed -n '/recovered range: binary_search/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
bs_score=0
echo "$bs_body" | grep -qE '\bwhile\b|\bfor\b' && bs_score=$(( bs_score + 1 ))  # has a loop
echo "$bs_body" | grep -qE '>> 1|/ 2'          && bs_score=$(( bs_score + 1 ))  # mid = (lo+hi)/2
echo "$bs_body" | grep -qE 'return'             && bs_score=$(( bs_score + 1 ))  # has return
if   [ "$bs_score" -ge 3 ]; then add_check "BinarySearchQuality" 5 5 "PASS"    "${bs_score}/3 patterns"
elif [ "$bs_score" -ge 2 ]; then add_check "BinarySearchQuality" 3 5 "PARTIAL" "${bs_score}/3 patterns"
else                              add_check "BinarySearchQuality" 0 5 "FAIL"    "${bs_score}/3 patterns"
fi

# ─── Check 16: Bubble Sort Quality ────────────────────────────────────────────
bs2_body=$(sed -n '/recovered range: bubble_sort/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
bsort_score=0
echo "$bs2_body" | grep -qE '\bwhile\b|\bfor\b' && bsort_score=$(( bsort_score + 1 ))  # outer loop
nested=$(echo "$bs2_body" | grep -cE '\bwhile\b|\bfor\b' 2>/dev/null || echo 0)
[ "${nested:-0}" -ge 2 ] && bsort_score=$(( bsort_score + 1 ))   # inner loop too
echo "$bs2_body" | grep -qE 'v[0-9]+\s*=\s*v[0-9]+' && bsort_score=$(( bsort_score + 1 ))  # swap
if   [ "$bsort_score" -ge 3 ]; then add_check "BubbleSortQuality" 5 5 "PASS"    "${bsort_score}/3 patterns"
elif [ "$bsort_score" -ge 2 ]; then add_check "BubbleSortQuality" 3 5 "PARTIAL" "${bsort_score}/3 patterns"
else                                 add_check "BubbleSortQuality" 0 5 "FAIL"    "${bsort_score}/3 patterns"
fi

# ─── Check 17: Pointer Walk Pattern ───────────────────────────────────────────
pw_body=$(sed -n '/recovered range: sum_ptr_walk/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
pw_score=0
echo "$pw_body" | grep -qE '\bwhile\b|\bfor\b' && pw_score=$(( pw_score + 1 ))
echo "$pw_body" | grep -qE '\+[0-9]|result\s*\+=' && pw_score=$(( pw_score + 1 ))
echo "$pw_body" | grep -qE 'return' && pw_score=$(( pw_score + 1 ))
if   [ "$pw_score" -ge 3 ]; then add_check "PtrWalkPattern" 5 5 "PASS"    "${pw_score}/3 patterns"
elif [ "$pw_score" -ge 2 ]; then add_check "PtrWalkPattern" 3 5 "PARTIAL" "${pw_score}/3 patterns"
else                              add_check "PtrWalkPattern" 0 5 "FAIL"    "${pw_score}/3 patterns"
fi

# ─── Check 18: Struct Field Access ────────────────────────────────────────────
# Linked list traversal: retdec emits *(uint64_t *)(v + 8) for ->next
ls_body=$(sed -n '/recovered range: list_sum/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
lc_body=$(sed -n '/recovered range: list_count/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
sf_score=0
echo "$ls_body" | grep -qE '\bwhile\b|\bfor\b' && sf_score=$(( sf_score + 1 ))
# Field access via ptr arithmetic (v1 + 8 or v2 + 8 etc.) or *(ptr + offset)
echo "$ls_body" | grep -qE '\*\s*\([^)]*\+\s*[0-9]+\)|\(v[0-9]+\s*\+\s*[0-9]' && sf_score=$(( sf_score + 1 ))
echo "$lc_body" | grep -qE '\bwhile\b|\bfor\b' && sf_score=$(( sf_score + 1 ))
if   [ "$sf_score" -ge 3 ]; then add_check "StructFieldAccess" 5 5 "PASS"    "${sf_score}/3 patterns"
elif [ "$sf_score" -ge 2 ]; then add_check "StructFieldAccess" 3 5 "PARTIAL" "${sf_score}/3 patterns"
else                              add_check "StructFieldAccess" 0 5 "FAIL"    "${sf_score}/3 patterns"
fi

# ─── Check 19: Memory Operations ──────────────────────────────────────────────
mallocs=$(cnt "$DECOMPILED" '\bmalloc\b')
frees=$(cnt   "$DECOMPILED" '\bfree\b')
mem_total=$(( mallocs + frees ))
if   [ "$mem_total" -ge 3 ]; then add_check "MemoryOps" 5 5 "PASS"    "${mallocs} malloc + ${frees} free"
elif [ "$mem_total" -ge 1 ]; then add_check "MemoryOps" 3 5 "PARTIAL" "${mallocs} malloc + ${frees} free"
else                               add_check "MemoryOps" 0 5 "FAIL"    "no malloc/free found"
fi

# ─── Check 20: Bit Operations ─────────────────────────────────────────────────
# pack_rgba / extract_channel / toggle_bits should use <<, >>, |, &
pr_body=$(sed -n '/recovered range: pack_rgba/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
ec_body=$(sed -n '/recovered range: extract_channel/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
tb_body=$(sed -n '/recovered range: toggle_bits/,/^}/p' "$DECOMPILED" 2>/dev/null || true)
bop_score=0
echo "$pr_body" | grep -qE '<<|>>' && bop_score=$(( bop_score + 1 ))
echo "$ec_body" | grep -qE '<<|>>|&' && bop_score=$(( bop_score + 1 ))
echo "$tb_body" | grep -qE '\^' && bop_score=$(( bop_score + 1 ))
if   [ "$bop_score" -ge 3 ]; then add_check "BitOperations" 5 5 "PASS"    "${bop_score}/3 bitwise funcs"
elif [ "$bop_score" -ge 2 ]; then add_check "BitOperations" 3 5 "PARTIAL" "${bop_score}/3 bitwise funcs"
else                               add_check "BitOperations" 0 5 "FAIL"    "${bop_score}/3 bitwise funcs"
fi

# =============================================================================
# C++ CHECKS (20 × 5 pts = 100 pts) — uses benchmark_cpp_O1.c
# =============================================================================

cpp_cnt() {
    [ -f "$DECOMPILED_CPP" ] || { echo 0; return; }
    grep -cE "$2" "$DECOMPILED_CPP" 2>/dev/null || echo 0
}

cpp_sed() {   # extract function body; $1=pattern $2=file
    [ -f "$DECOMPILED_CPP" ] || { echo ""; return; }
    sed -n "/${1}/,/^}/p" "$DECOMPILED_CPP" 2>/dev/null || true
}

if [ ! -f "$DECOMPILED_CPP" ]; then
    echo "NOTE: $DECOMPILED_CPP not found — C++ checks will score 0."
    echo "      Run: bash tests/benchmark/decompile_cpp.sh"
    for i in $(seq 1 20); do
        add_check "Cpp_check_${i}" 0 5 "SKIP" "cpp output missing"
    done
else

# ─── C++ Check 01: Class Method Demangling ────────────────────────────────────
# Expect Circle::, Rectangle::, Triangle:: method names demangled
cls_methods=$(cpp_cnt X 'Circle::|Rectangle::|Triangle::|Animal::|Dog::|Cat::|Bird::')
if   [ "$cls_methods" -ge 10 ]; then add_check "CxxClassMethods"    5 5 "PASS"    "${cls_methods} class::method(s)"
elif [ "$cls_methods" -ge  5 ]; then add_check "CxxClassMethods"    3 5 "PARTIAL" "${cls_methods} class::method(s)"
else                                  add_check "CxxClassMethods"    0 5 "FAIL"    "${cls_methods} class::method(s)"
fi

# ─── C++ Check 02: Vtable Structure Recovery ──────────────────────────────────
vtables=$(cpp_cnt X 'struct vtable_[0-9a-f]+_type')
if   [ "$vtables" -ge 4 ]; then add_check "VtableRecovery"      5 5 "PASS"    "${vtables} vtable struct(s)"
elif [ "$vtables" -ge 2 ]; then add_check "VtableRecovery"      3 5 "PARTIAL" "${vtables} vtable struct(s)"
else                             add_check "VtableRecovery"      0 5 "FAIL"    "${vtables} vtable struct(s)"
fi

# ─── C++ Check 03: Dynamic Cast Recovery ──────────────────────────────────────
dcast=$(cpp_cnt X '__dynamic_cast|dynamic_cast')
if   [ "$dcast" -ge 4 ]; then add_check "DynamicCastRecovery"  5 5 "PASS"    "${dcast} dynamic_cast(s)"
elif [ "$dcast" -ge 1 ]; then add_check "DynamicCastRecovery"  3 5 "PARTIAL" "${dcast} dynamic_cast(s)"
else                           add_check "DynamicCastRecovery"  0 5 "FAIL"    "no dynamic_cast"
fi

# ─── C++ Check 04: Exception Handling Recovery ────────────────────────────────
eh=$(cpp_cnt X '__cxa_throw|__cxa_begin_catch|__cxa_end_catch|__gxx_personality')
if   [ "$eh" -ge 5 ]; then add_check "ExceptionHandling"    5 5 "PASS"    "${eh} EH pattern(s)"
elif [ "$eh" -ge 2 ]; then add_check "ExceptionHandling"    3 5 "PARTIAL" "${eh} EH pattern(s)"
else                        add_check "ExceptionHandling"    0 5 "FAIL"    "no EH patterns"
fi

# ─── C++ Check 05: Constructor / Destructor Demangling ────────────────────────
ctordtor=$(cpp_cnt X '~Circle|~Rectangle|~Triangle|~Animal|~Dog|~Cat|~Bird|~Buffer|~Shape')
if   [ "$ctordtor" -ge 4 ]; then add_check "CtorDtorNames"       5 5 "PASS"    "${ctordtor} dtor(s) named"
elif [ "$ctordtor" -ge 2 ]; then add_check "CtorDtorNames"       3 5 "PARTIAL" "${ctordtor} dtor(s) named"
else                              add_check "CtorDtorNames"       0 5 "FAIL"    "${ctordtor} dtor(s) named"
fi

# ─── C++ Check 06: Manual Sort Algorithm Names ────────────────────────────────
msorts=$(cpp_cnt X 'heapify|heap_sort|insertion_sort|quicksort_partition')
if   [ "$msorts" -ge 4 ]; then add_check "ManualSortNames"      5 5 "PASS"    "${msorts} sort fn(s)"
elif [ "$msorts" -ge 2 ]; then add_check "ManualSortNames"      3 5 "PARTIAL" "${msorts} sort fn(s)"
else                            add_check "ManualSortNames"      0 5 "FAIL"    "${msorts} sort fn(s)"
fi

# ─── C++ Check 07: Mutual Recursion (IPA) ─────────────────────────────────────
is_even_n=$(cpp_cnt X 'is_even')
is_odd_n=$(cpp_cnt X 'is_odd')
mutual=$(( is_even_n + is_odd_n ))
if   [ "$mutual" -ge 8 ]; then add_check "MutualRecursionIPA"   5 5 "PASS"    "is_even:${is_even_n} is_odd:${is_odd_n}"
elif [ "$mutual" -ge 4 ]; then add_check "MutualRecursionIPA"   3 5 "PARTIAL" "is_even:${is_even_n} is_odd:${is_odd_n}"
else                            add_check "MutualRecursionIPA"   0 5 "FAIL"    "is_even:${is_even_n} is_odd:${is_odd_n}"
fi

# ─── C++ Check 08: Deep Recursion (Ackermann) ─────────────────────────────────
ack=$(cpp_cnt X 'ackermann')
if   [ "$ack" -ge 6 ]; then add_check "DeepRecursionIPA"    5 5 "PASS"    "${ack} ackermann ref(s)"
elif [ "$ack" -ge 2 ]; then add_check "DeepRecursionIPA"    3 5 "PARTIAL" "${ack} ackermann ref(s)"
else                         add_check "DeepRecursionIPA"    0 5 "FAIL"    "ackermann not found"
fi

# ─── C++ Check 09: STL Algorithm Wrapper Names ────────────────────────────────
stl_wrappers=$(cpp_cnt X 'stl_accumulate|stl_transform|stl_for_each|stl_find|stl_partition|stl_copy|stl_all_|stl_count_|stl_sort')
if   [ "$stl_wrappers" -ge 8 ]; then add_check "STLAlgoWrappers"    5 5 "PASS"    "${stl_wrappers} stl wrapper(s)"
elif [ "$stl_wrappers" -ge 4 ]; then add_check "STLAlgoWrappers"    3 5 "PARTIAL" "${stl_wrappers} stl wrapper(s)"
else                                  add_check "STLAlgoWrappers"    0 5 "FAIL"    "${stl_wrappers} stl wrapper(s)"
fi

# ─── C++ Check 10: Red-Black Tree (std::map internals) ────────────────────────
rbtree=$(cpp_cnt X '_Rb_tree|_M_get_insert|_M_lower_bound')
if   [ "$rbtree" -ge 3 ]; then add_check "RBTreeOperations"    5 5 "PASS"    "${rbtree} rb-tree pattern(s)"
elif [ "$rbtree" -ge 1 ]; then add_check "RBTreeOperations"    3 5 "PARTIAL" "${rbtree} rb-tree pattern(s)"
else                            add_check "RBTreeOperations"    0 5 "FAIL"    "no rb-tree patterns"
fi

# ─── C++ Check 11: Hash Table (std::unordered_map internals) ──────────────────
htable=$(cpp_cnt X '_Hashtable|_M_find_before_node|_M_rehash')
if   [ "$htable" -ge 2 ]; then add_check "HashTableOps"        5 5 "PASS"    "${htable} hash-table pattern(s)"
elif [ "$htable" -ge 1 ]; then add_check "HashTableOps"        3 5 "PARTIAL" "${htable} hash-table pattern(s)"
else                            add_check "HashTableOps"        0 5 "FAIL"    "no hash-table patterns"
fi

# ─── C++ Check 12: Mutex / Lock Patterns ──────────────────────────────────────
mutpat=$(cpp_cnt X 'mutex|lock_guard|pthread_mutex|__pthread_mutex')
if   [ "$mutpat" -ge 4 ]; then add_check "MutexLockPatterns"   5 5 "PASS"    "${mutpat} mutex pattern(s)"
elif [ "$mutpat" -ge 1 ]; then add_check "MutexLockPatterns"   3 5 "PARTIAL" "${mutpat} mutex pattern(s)"
else                            add_check "MutexLockPatterns"   0 5 "FAIL"    "no mutex patterns"
fi

# ─── C++ Check 13: Atomic / CAS Patterns ──────────────────────────────────────
atompat=$(cpp_cnt X 'atomic_fetch_add_loop|compare_exchange|fetch_add')
if   [ "$atompat" -ge 3 ]; then add_check "AtomicCASPatterns"  5 5 "PASS"    "${atompat} atomic pattern(s)"
elif [ "$atompat" -ge 1 ]; then add_check "AtomicCASPatterns"  3 5 "PARTIAL" "${atompat} atomic pattern(s)"
else                             add_check "AtomicCASPatterns"  0 5 "FAIL"    "no atomic patterns"
fi

# ─── C++ Check 14: new / delete Recovery ──────────────────────────────────────
newdel=$(cpp_cnt X '_Znw|_Zna|_Zdl|_Zda|operator new|operator delete')
if   [ "$newdel" -ge 6 ]; then add_check "NewDeleteRecovery"   5 5 "PASS"    "${newdel} new/delete ref(s)"
elif [ "$newdel" -ge 2 ]; then add_check "NewDeleteRecovery"   3 5 "PARTIAL" "${newdel} new/delete ref(s)"
else                            add_check "NewDeleteRecovery"   0 5 "FAIL"    "no new/delete patterns"
fi

# ─── C++ Check 15: Demangled Name Count ───────────────────────────────────────
demangled=$(cpp_cnt X '// Demangled:')
if   [ "$demangled" -ge 80 ]; then add_check "DemangledNames"  5 5 "PASS"    "${demangled} demangled"
elif [ "$demangled" -ge 30 ]; then add_check "DemangledNames"  3 5 "PARTIAL" "${demangled} demangled"
else                                add_check "DemangledNames"  0 5 "FAIL"    "${demangled} demangled"
fi

# ─── C++ Check 16: RTTI type_info Patterns ────────────────────────────────────
rttiref=$(cpp_cnt X 'type_info|typeid|__cxa_typeid')
if   [ "$rttiref" -ge 4 ]; then add_check "RTTITypeInfo"       5 5 "PASS"    "${rttiref} RTTI ref(s)"
elif [ "$rttiref" -ge 1 ]; then add_check "RTTITypeInfo"       3 5 "PARTIAL" "${rttiref} RTTI ref(s)"
else                             add_check "RTTITypeInfo"       0 5 "FAIL"    "no RTTI patterns"
fi

# ─── C++ Check 17: std::string SSO / operations ───────────────────────────────
strops=$(cpp_cnt X 'basic_string|_M_construct|_M_append|_M_create|_M_replace')
if   [ "$strops" -ge 20 ]; then add_check "StringSSO"          5 5 "PASS"    "${strops} string op(s)"
elif [ "$strops" -ge  5 ]; then add_check "StringSSO"          3 5 "PARTIAL" "${strops} string op(s)"
else                             add_check "StringSSO"          0 5 "FAIL"    "${strops} string op(s)"
fi

# ─── C++ Check 18: Exception Class Hierarchy ──────────────────────────────────
excl=$(cpp_cnt X 'DomainError|out_of_range|invalid_argument|runtime_error|exception')
if   [ "$excl" -ge 8 ]; then add_check "ExceptionClasses"    5 5 "PASS"    "${excl} exc class ref(s)"
elif [ "$excl" -ge 3 ]; then add_check "ExceptionClasses"    3 5 "PARTIAL" "${excl} exc class ref(s)"
else                          add_check "ExceptionClasses"    0 5 "FAIL"    "${excl} exc class ref(s)"
fi

# ─── C++ Check 19: Quicksort Recursive Pattern ────────────────────────────────
qs_body=$(cpp_sed 'Demangled:.*quicksort(' "$DECOMPILED_CPP")
qs_score=0
echo "$qs_body" | grep -qE '\bquicksort\b' && qs_score=$(( qs_score + 1 ))   # calls itself
echo "$qs_body" | grep -qE 'quicksort_partition' && qs_score=$(( qs_score + 1 ))
echo "$qs_body" | grep -qE '\bif\b' && qs_score=$(( qs_score + 1 ))
if   [ "$qs_score" -ge 3 ]; then add_check "QuicksortRecursion"  5 5 "PASS"    "${qs_score}/3 patterns"
elif [ "$qs_score" -ge 2 ]; then add_check "QuicksortRecursion"  3 5 "PARTIAL" "${qs_score}/3 patterns"
else                              add_check "QuicksortRecursion"  0 5 "FAIL"    "${qs_score}/3 patterns"
fi

# ─── C++ Check 20: Total C++ Function Count ───────────────────────────────────
cppfuncs=$(cpp_cnt X '^[a-zA-Z_][a-zA-Z0-9_ *&]*\s+[_A-Za-z][^;{]*\(')
if   [ "$cppfuncs" -ge 200 ]; then add_check "CppFunctionCount"  5 5 "PASS"    "${cppfuncs} functions"
elif [ "$cppfuncs" -ge 80  ]; then add_check "CppFunctionCount"  3 5 "PARTIAL" "${cppfuncs} functions"
else                                add_check "CppFunctionCount"  0 5 "FAIL"    "${cppfuncs} functions"
fi

fi  # end if DECOMPILED_CPP exists

# ─── Read timing ──────────────────────────────────────────────────────────────
TIMING_FILE="$OUT_DIR/timing.txt"

# ─── Print human-readable summary ────────────────────────────────────────────
echo ""
echo "======================================================="
echo " RetDec Benchmark Score  —  $(date '+%Y-%m-%d %H:%M:%S')"
echo "======================================================="
printf "  %-28s %5s / %-5s  %s\n" "Check" "Score" "Max" "Status"
echo "  ─────────────────────────────────────────────────────"
echo "  --- C benchmark (100 pts) ---"
c_score=0
for i in "${!check_names[@]}"; do
    [ "$i" -lt 20 ] || continue
    c_score=$(( c_score + check_scores[$i] ))
    printf "  %-28s %5d / %-5d  [%s]  %s\n" \
        "${check_names[$i]}" "${check_scores[$i]}" "${check_max[$i]}" \
        "${check_status[$i]}" "${check_notes[$i]}"
done
printf "  %-28s %5d / %-5d\n" "C subtotal" "$c_score" "100"
echo "  ─────────────────────────────────────────────────────"
echo "  --- C++ benchmark (100 pts) ---"
cpp_score=0
for i in "${!check_names[@]}"; do
    [ "$i" -ge 20 ] || continue
    cpp_score=$(( cpp_score + check_scores[$i] ))
    printf "  %-28s %5d / %-5d  [%s]  %s\n" \
        "${check_names[$i]}" "${check_scores[$i]}" "${check_max[$i]}" \
        "${check_status[$i]}" "${check_notes[$i]}"
done
printf "  %-28s %5d / %-5d\n" "C++ subtotal" "$cpp_score" "100"
echo "  ─────────────────────────────────────────────────────"
printf "  %-28s %5d / %-5d\n" "TOTAL" "$total_score" "$max_score"
echo "======================================================="
if [ -f "$TIMING_FILE" ]; then
    echo ""
    echo " Decompiler timing:"
    cat "$TIMING_FILE" | sed 's/^/   /'
fi
echo ""

# ─── Emit JSON ────────────────────────────────────────────────────────────────
{
    echo "{"
    echo "  \"timestamp\": \"$(date -u '+%Y-%m-%dT%H:%M:%SZ')\","
    echo "  \"total_score\": $total_score,"
    echo "  \"max_score\": $max_score,"
    echo "  \"checks\": ["
    n=${#check_names[@]}
    for i in "${!check_names[@]}"; do
        comma=""
        [ $((i+1)) -lt "$n" ] && comma=","
        echo "    {"
        echo "      \"name\": \"${check_names[$i]}\","
        echo "      \"score\": ${check_scores[$i]},"
        echo "      \"max\": ${check_max[$i]},"
        echo "      \"status\": \"${check_status[$i]}\","
        echo "      \"note\": \"${check_notes[$i]}\""
        echo "    }${comma}"
    done
    echo "  ],"
    echo "  \"timing\": {"
    if [ -f "$TIMING_FILE" ]; then
        first=1
        while IFS=': ' read -r label ms_str _; do
            [ "$first" -eq 0 ] && printf ','
            printf '\n    "%s": %s' "$label" "${ms_str%ms}"
            first=0
        done < "$TIMING_FILE"
        echo ""
    fi
    echo "  }"
    echo "}"
} > "$RESULT_JSON"

echo "Score card written to: $RESULT_JSON"
