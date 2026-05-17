# RetDec Decompiler Benchmark

A self-contained benchmark suite that measures the **quality and performance** of
the RetDec decompilation pipeline.  It provides a stable, reproducible score that
can be used to detect regressions and measure improvement over time.

---

## Quick Start (WSL)

```bash
# One shot — compile, decompile, score, compare against baseline
bash tests/benchmark/run_full_benchmark.sh

# First-time baseline establishment
bash tests/benchmark/run_full_benchmark.sh --save-baseline

# C++ decompilation only (takes ~60s — run separately)
bash tests/benchmark/decompile_cpp.sh
```

---

## Files

| File | Purpose |
|------|---------|
| `benchmark.c` | C source — 28 sections covering all C-level optimizers |
| `benchmark_cpp.cpp` | C++ source — 20 sections covering C++/STL/concurrency modules |
| `run_benchmark.sh` | Compile all variants (C: O1/O2 × stripped; C++: O1 × stripped) and decompile |
| `decompile_cpp.sh` | Standalone C++ decompilation (~60 s per variant) |
| `score_benchmark.sh` | Analyse decompiled output → `score.json` (200 pts total) |
| `compare_benchmark.sh` | Diff a new score against `baseline.json` |
| `run_full_benchmark.sh` | Orchestrates all scripts in sequence |
| `baseline.json` | Saved reference score (committed to repo) |

---

## Benchmark Sections (`benchmark.c`)

| § | Name | Optimizer(s) targeted |
|---|------|----------------------|
| 1 | `day_name`, `classify_char` | IfToSwitchOptimizer |
| 2 | `sum_range`, `count_positive` | WhileTrueToForLoopOptimizer, VarDefForLoopOptimizer |
| 3 | `find_first_zero` | BreakContinueReturnOptimizer |
| 4 | `copy_prop_demo`, `copy_prop_chain` | CopyPropagationOptimizer |
| 5 | `dead_assign_demo` | DeadLocalAssignOptimizer |
| 6 | `mba_add`, `mba_sub`, `arith_identity` | SimplifyArithmExprOptimizer, MBASubOptimizer |
| 7 | `bit_and_as_logic`, `bit_or_as_logic`, `bit_xor_check` | BitOpToLogOpOptimizer |
| 8 | `shift_mul`, `shift_div` | BitShiftOptimizer |
| 9 | `widen_i16`, `chained_cast`, `sat_add_u8` | CCastOptimizer, CastSimplifierOptimizer |
| 10 | `deref_to_index`, `deref_write` | DerefToArrayIndexOptimizer |
| 11 | `deref_addr_demo` | DerefAddressOptimizer |
| 12 | `classify_range`, `nested_if_flatten` | IfStructureOptimizer |
| 13 | `find_nonzero` | GotoCFGOptimizer |
| 14 | `vardef_placement` | VarDefStmtOptimizer |
| 15 | `sum_array` | VarDefForLoopOptimizer |
| 16 | `no_init_cleanup` | NoInitVarDefOptimizer |
| 17 | `list_sum`, `list_count` | UnknownTypeInferrer |
| 18 | `self_assign_demo` | SelfAssignOptimizer |
| 19 | `empty_stmts` | EmptyStmtOptimizer |
| 20 | `fibonacci`, `factorial`, `gcd` | Call-graph / recursion recovery |
| 21 | `matrix_search`, `count_even_rows` | CFG reconstruction (nested loops) |
| 22 | `bubble_sort`, `binary_search` | Loop + conditional quality |
| 23 | `alloc_and_fill`, `sum_ptr_walk` | Pointer arithmetic / dynamic allocation |
| 24 | `my_strlen`, `my_strcmp`, `my_strcpy` | CharArrayToStringOptimizer |
| 25 | `pack_rgba`, `extract_channel`, `toggle_bits` | Bitfield / bitmask patterns |
| 26 | `wide_mul`, `clamp_u16`, `abs_val`, `sign_of` | Integer type width recovery |
| 27 | `add`, `sub`, `mul`, `apply_op`, `dispatch` | Function pointer table dispatch |
| 28 | `safe_divide`, `validate_and_process` | IfStructureOptimizer (guard clauses) |

---

## C Scoring Rubric (100 pts — `benchmark_O1.c`)

Each of the 20 C checks is worth **5 points** (PASS=5, PARTIAL=3, FAIL=0):

| # | Check | What is measured |
|---|-------|-----------------|
| 01 | FuncNameRecovery | 12 key function names present in output |
| 02 | LoopRecovery | ≥12 while/for loops reconstructed |
| 03 | BitShiftOptimizer | `shift_mul → << 3`, `shift_div → /N` |
| 04 | MBASimplify | `mba_add` fully simplified to `a + b` |
| 05 | SelfAssignRemoval | Zero `x = x;` patterns remain |
| 06 | ArithIdentities | Zero `+0`, `*1`, `-0` patterns remain |
| 07 | GotoCFGOptimizer | Zero (or minimal) `goto` statements |
| 08 | CastRemoval | Cast density ≤40% of statements |
| 09 | GuardClausePattern | ≥15 early-return if-guards present |
| 10 | NoInitVarDef | ≤5 bare `int x;` declarations |
| 11 | TypeInference | ≥30 concrete integer types |
| 12 | RecursionQuality | `fibonacci`, `factorial`, `gcd` call themselves |
| 13 | FuncPtrDispatch | `dispatch`/`apply_op` patterns present |
| 14 | StringLiterals | ≥4 string constants recovered |
| 15 | BinarySearchQuality | Loop + midpoint calculation + return |
| 16 | BubbleSortQuality | Outer loop + inner loop + swap |
| 17 | PtrWalkPattern | `sum_ptr_walk` loop + accumulate + return |
| 18 | StructFieldAccess | `list_sum`/`list_count` field-offset reads |
| 19 | MemoryOps | `malloc` and `free` correctly preserved |
| 20 | BitOperations | `<<`/`>>`/`&`/`^` in bitwise functions |

---

## C++ Benchmark Sections (benchmark_cpp.cpp)

| § | Name | Module(s) targeted |
|---|------|-------------------|
| 1 | `Circle`, `Rectangle`, `Triangle`, `Shape` (virtual dispatch) | `cxx_backend::VtableDetector` |
| 2 | `Buffer` (ctor/dtor, `new[]`/`delete[]`) | `cxx_backend::CtorDtorDetector`, new/delete recovery |
| 3 | `shape_type_name`, `dispatch_by_type` (dynamic_cast, typeid) | `rtti-finder`, RTTI recovery |
| 4 | `DomainError`, `safe_sqrt`, `try_sqrt`, `exception_chain` | `eh_reconstruct` (Itanium EH) |
| 5 | `build_vec`, `vec_sum`, `vec_filter_inplace` | `container_detect::VectorDetector` |
| 6 | `build_list`, `list_sum_stl` | `container_detect::ListDetector` |
| 7 | `word_count`, `map_total` | `container_detect::MapDetector` (red-black tree) |
| 8 | `square_map`, `umap_lookup` | `container_detect::UnorderedMapDetector` |
| 9 | `build_greeting`, `count_vowels`, `reverse_string` | `container_detect::StringDetector`, SSO |
| 10 | `build_shared_list`, `shared_list_sum` | `container_detect::SharedPtrDetector` |
| 11 | `stl_accumulate`, `stl_transform_double`, `stl_for_each_print` | `algo_recover` (Accumulate, Transform, ForEach) |
| 12 | `stl_find_first`, `stl_find_if_even`, `stl_partition_evens` | `algo_recover` (Find, FindIf, Partition) |
| 13 | `stl_copy_positive`, `stl_all_positive`, `stl_count_even` | `algo_recover` (Copy, AllOf, Count) |
| 14 | `stl_sort_copy`, `stl_stable_sort_copy` | `sort_detect` (IntrosortDetector, MergesortDetector) |
| 15 | `insertion_sort`, `heap_sort`, `quicksort` | `sort_detect` (InsertionSort, Heapsort, Quicksort) |
| 16 | `mutex_increment`, `atomic_fetch_add_loop`, `cas_spinlock_demo` | `concurrency_detect` |
| 17 | `tpl_max`, `tpl_clamp`, `tpl_sum` (templates) | `type_inference`, `cxx_backend::TemplateSkeleton` |
| 18 | `is_even`/`is_odd` mutual recursion, `ackermann` | `ipa` (call_graph, Tarjan SCC) |
| 19 | `Vec2` operator overloading | `cxx_backend` (operator recovery, field inference) |
| 20 | `Animal`/`Mammal`/`Dog`/`Cat`/`Bird` hierarchy | `cxx_backend` (multi-level inheritance) |

## C++ Scoring Rubric (100 pts — `benchmark_cpp_O1.c`)

| # | Check | What is measured |
|---|-------|-----------------|
| 01 | CxxClassMethods | ≥10 `Class::method()` names demangled |
| 02 | VtableRecovery | ≥4 vtable struct types with fn-pointer entries |
| 03 | DynamicCastRecovery | ≥4 `__dynamic_cast` call sites |
| 04 | ExceptionHandling | ≥5 `__cxa_throw`/`__cxa_begin_catch` references |
| 05 | CtorDtorNames | ≥4 destructor names demangled (`~Circle` etc.) |
| 06 | ManualSortNames | ≥4 sort function names demangled (`heapify`, `quicksort_partition`, etc.) |
| 07 | MutualRecursionIPA | ≥8 combined `is_even`/`is_odd` references |
| 08 | DeepRecursionIPA | ≥6 `ackermann` references (recursive self-call) |
| 09 | STLAlgoWrappers | ≥8 `stl_` wrapper function names demangled |
| 10 | RBTreeOperations | ≥3 `_Rb_tree` (std::map) internal function patterns |
| 11 | HashTableOps | ≥2 `_Hashtable` (std::unordered_map) internal patterns |
| 12 | MutexLockPatterns | ≥4 mutex/lock references |
| 13 | AtomicCASPatterns | ≥3 atomic operation references |
| 14 | NewDeleteRecovery | ≥6 `_Znw`/`_Zdl`/operator new/delete references |
| 15 | DemangledNames | ≥80 demangled function name comments |
| 16 | RTTITypeInfo | ≥4 `type_info` or `typeid` references |
| 17 | StringSSO | ≥20 `basic_string`/`_M_construct`/`_M_append` references |
| 18 | ExceptionClasses | ≥8 exception class name references |
| 19 | QuicksortRecursion | quicksort body: recursive call + `quicksort_partition` + if |
| 20 | CppFunctionCount | ≥200 functions recovered (including inlined STL) |

---

## Baseline (established 2026-04-03)

| Metric | Value |
|--------|-------|
| **Total Score** | **196 / 200** |
| **C Score** | **96 / 100** |
| **C++ Score** | **100 / 100** |
| C PASS checks | 18 / 20 |
| C PARTIAL checks | 2 / 20 (GotoCFG, CastRemoval) |
| C++ PASS checks | 20 / 20 |
| C O1 decompile time | ~4.7 s |
| C O2 decompile time | ~4.8 s |
| C++ O1 decompile time | ~59 s (STL templates inlined) |

### Known C Partials

**GotoCFGOptimizer (3/5):** Two `goto` statements remain in a complex multi-exit
loop inside `binary_search` / `fibonacci` area.  This is a genuine edge case in
retdec's unstructured-CFG recovery — the GotoCFG optimizer cannot always eliminate
all gotos from multi-exit loops.

**CastRemoval (3/5):** Cast density is 43%.  For stripped x86-64 binaries, retdec
inserts many `(uint32_t)` and `(int32_t)` truncation casts because all registers
are 64-bit.  Reducing this requires deeper type-range analysis in the lifter.

---

## Regression Workflow

```bash
# After making changes, re-run and compare:
bash tests/benchmark/run_full_benchmark.sh

# Exit code 0 = no regression, 1 = regression detected
echo "Exit: $?"
```

`compare_benchmark.sh` prints a delta table.  Any check that drops points triggers
an exit code of 1, making it suitable for CI pipelines.

---

## Design Notes

- All benchmark functions use `__attribute__((noinline))` to prevent GCC inlining.
- `-mno-sse -mno-sse2` is added for C O2 builds to prevent SSE constants that crash
  retdec's capstone lifter (`translateSseMovLane0`).
- `volatile` parameters prevent GCC constant-folding loop bounds away.
- `g_sink` / `g_sink_cpp` absorb all return values so the linker keeps every function.
- The C++ binary takes ~59 s to decompile because `std::sort`, `std::stable_sort`,
  and `std::map` inline thousands of lines of STL template instantiations.  This is
  intentional — it exercises the decompiler at scale.
