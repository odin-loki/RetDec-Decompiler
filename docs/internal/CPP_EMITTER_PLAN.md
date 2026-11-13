# C++ emitter plan: `--output-lang cpp` with full STL support

Status: **plan** (nothing below is shipped yet). Owner IDs: `LLVM-22`,
`DEAD-01`, `CLI-01` in [Plan.md](../../Plan.md).

## Goal

Native binaries can be decompiled to **C++** as a first-class choice next to C,
with idiomatic, compilable output:

- `--output-lang cpp` (aliases `c++`, `cxx`) on the CLI, the same choice in the
  GUI settings, and `decompParams.outputLang = "cpp"` in the config JSON. C
  stays the default.
- Output that **compiles** with a stock C++23 compiler (`clang++`/`g++`/MSVC),
  in the same three shapes C has today: `.cpp`, plus a buildable
  `.hpp` + `_stubs.cpp` + `.buildable.cpp` set.
- Classes, inheritance, virtual calls, constructors/destructors, `new`/`delete`,
  exceptions and namespaces recovered from the binary.
- **Full STL support**: every standard container, string, smart pointer,
  utility and algorithm that the binary was compiled against is shown as the
  STL type and call (`std::vector<int32_t> v; v.push_back(x);`), for
  libstdc++, libc++ and the MSVC STL, on 32- and 64-bit targets. The
  ["STL coverage"](#stl-coverage) section defines what "supported" means.
- Nothing is invented: a rewrite is made only on recorded evidence. Without
  evidence the output falls back to plain C-in-C++, never to a guess.

## Where things stand

| Piece | State | Path |
|---|---|---|
| CLI | `parseOutputLang("cpp")` **throws** ("pending LLVM-22 / cxx_backend"); `OutputLangId::Cpp`, `.cpp` extension and `nativeTargetHllId` (→ `"c"`) already exist | `src/retdec-decompiler/output_lang.cpp` |
| GUI | `coerceNativeOutputLang()` silently maps `cpp`/`c++`/`cxx` → `c` | `src/gui/model/settings.cpp` |
| Shipped writer | `CHLLWriter`, 2,900 lines, registered with `REGISTER_AT_FACTORY("c", …)`; it writes from llvmir2hll's BIR | `src/llvmir2hll/hll/hll_writers/c_hll_writer.cpp` |
| Previous C++ attempt | `cxx_backend`: a C++ AST (classes, templates, enums, namespaces, casts, new/delete, try/catch, lambdas), an emitter, and lifters (vtable, ctor/dtor, new/delete, EH, namespace grouping). 1,678 lines, 51 unit tests, **zero consumers** | `src/cxx_backend/`, `include/retdec/cxx_backend/`, `tests/cxx_backend/` |
| Alternate C emitter | `codegen` (goto elimination, loop forming, condition normalising), also not on the shipped path | `src/codegen/` |
| Class evidence | Class hierarchy (vtables, ctors/dtors, superclasses) written to the config | `src/bin2llvmir/optimizations/class_hierarchy/`, `include/retdec/common/class.h` |
| RTTI | GCC/Itanium and MSVC RTTI and vtable finders | `src/rtti-finder/` |
| Names | Itanium, Microsoft and Borland demanglers | `src/demangler/` |
| STL shapes | Detectors for vector, string, map/set (red-black tree), list, unordered map/set, open addressing, ring buffer/deque, smart pointers, optional, variant, array. Today they only **label** the sidecar and add `// [RetDec]` comments | `src/container_detect/`, `src/retdec/semantic_recovery_export.cpp` |
| Algorithms | sort, find, accumulate, for_each, transform, partition, binary search, iterator recovery; same: labels only | `src/algo_recover/` |
| Compile gate for C | CC-01, "emitted C compiles", over the 252-binary corpus | `scripts/ci/check_emitted_c_compiles.sh`, `ctest-linux.yml` |

### Why the first attempt stalled

1. **Wrong IR.** `cxx_backend` lifts from `codegen::CUnit`. The shipped pipeline
   never builds one; it goes LLVM IR → BIR → `CHLLWriter`. Wiring it in would
   have meant a second, unvalidated decompiler back half.
2. **No compile check.** Nothing compiled what it emitted. Its tests check
   substrings (`"new"`, `"int32_t"`), not that the output is C++.
3. **The STL was only metadata.** The container and algorithm detectors fill
   the sidecar but never reach the type system or the writer, so the code
   still shows `*(int32_t *)(v + 8)`.
4. **The CLI shipped renamed C as C++** before any of this worked; `CLI-01`
   then had to reject `cpp`, and the work stopped there.

The plan below fixes each of these in that order.

## Architecture

```
LLVM IR ──► BIR (llvmir2hll) ──► C++ lowering passes ──► CppHLLWriter ──► .cpp / .hpp
                │                     ▲   ▲   ▲
                │                     │   │   └─ STL binding (container_detect, algo_recover, ABI tables)
                │                     │   └───── class model (class_hierarchy, rtti-finder, demangler)
                └─ CHLLWriter (C)     └───────── EH model (LSDA / FuncInfo)
```

- **`CppHLLWriter`** is a new writer in `src/llvmir2hll/hll/hll_writers/`,
  derived from `CHLLWriter` and registered as `"cpp"`. It reuses everything
  C-shaped (statements, expressions, operator precedence, bracket and
  compound-op managers) and overrides what C++ spells differently.
- **`CxxModel`** is a side table attached to the BIR `Module`: classes and
  their layouts, method ↔ function mapping, `this` parameters, virtual slots,
  namespaces, EH regions, and STL type bindings for variables, struct fields
  and parameters. The lowering passes fill it, and the writer only reads it.
  BIR itself is not forked.
- **C++ lowering passes** (in `src/llvmir2hll/optimizer/optimizers/cxx/`) run
  only for `cpp`. Each one turns BIR patterns into model facts or BIR
  rewrites, and records the evidence for each.
- **`cxx_backend` is ported, not wired.** Its AST printing helpers and the
  detector heuristics (`VtableDetector`, `CtorDtorDetector`,
  `NewDeleteRecovery`, `EhRecovery`, `NamespaceGrouper`) are rewritten
  against BIR and the `CxxModel`, and its 51 tests move with them. Once
  nothing uses the `codegen::CUnit` versions, the tree is deleted (`DEAD-01`
  resolved).

## STL support

### Three tiers of evidence

| Tier | Input | What it recovers | Confidence |
|---|---|---|---|
| **1. Symbols** | Demangled imports, exports and debug or symbol-table names (`std::vector<int, std::allocator<int>>::_M_realloc_insert`, `std::__1::basic_string…`, MSVC `?push_back@?$vector@H…`) | Exact template arguments; calls become member calls on a typed object | High; types come from the mangling |
| **2. Structure** | `container_detect` and `algo_recover` results, checked against the **ABI layout tables** below | Container type and element type from layout and access pattern; inlined algorithms | Medium; scored and thresholded |
| **3. None** | — | Nothing. Plain C-in-C++ with `reinterpret_cast`, as the C writer would write it | — |

Every STL rewrite stores `(tier, detector, score, evidence addresses)` in the
`CxxModel` and the sidecar. The user chooses how far to go with
`--cpp-stl=off|symbols|safe|aggressive`, default `safe`:

- `symbols`: tier 1 only.
- `safe`: tier 1, plus tier 2 at or above the precision threshold measured in
  STL-01.
- `aggressive`: lower thresholds; clearly marked in the output.

### ABI layout tables

One data file per standard library × pointer size
(`support/cpp/stl_abi/{libstdcxx,libcxx,msvc}-{32,64}.json`), naming each
member's offset, size and role. Detection and rewriting both read these
tables; nothing is hard-coded in passes. Initial content:

- `vector<T>`: begin / end / end-of-storage.
- `basic_string`:
  - libstdc++: pointer + size + 16-byte SSO buffer.
  - libc++: short/long union with a flag bit.
  - MSVC: `_Bx` union + size + capacity.
- `map/set/multimap/multiset`: tree header, node colour/parent/left/right,
  value offset.
- `unordered_*`: bucket array, bucket count, before-begin node, element count,
  max load factor; node layout with and without a cached hash.
- `list`, `forward_list`: sentinel node, next/prev, size.
- `deque`: block map, start/finish iterators, block size per element size.
- `shared_ptr`/`weak_ptr`: object pointer + control block (use/weak counts,
  vtable); `make_shared` inline block.
- `unique_ptr` (empty-deleter EBO), `optional`, `variant` (index width),
  `function` (SBO size), `string_view`, `span`, `array`, `pair`, `tuple`,
  `bitset`.

### STL coverage

"Supported" means, for each component, all three of:

1. **Typed**: variables, fields, parameters and returns show the STL type
   with template arguments.
2. **Operations**: inlined or called operations show as the member or free
   function. For example:
   - `size()`, `empty()`, `data()`, `operator[]`, `at`, `push_back`,
     `emplace_back`, `insert`, `erase`, `find`, `clear`, `reserve`,
     `resize`, iteration;
   - grow/realloc paths fold into the operation that triggered them;
   - destructors and frees of the members fold into scope end.
3. **Measured**: STL-01 precision and recall are reported for it on the
   corpus, per standard library.

Components:

- **Containers**: `vector`, `array`, `deque`, `list`, `forward_list`, `map`,
  `multimap`, `set`, `multiset`, `unordered_map`, `unordered_multimap`,
  `unordered_set`, `unordered_multiset`, `stack`, `queue`, `priority_queue`
  (adaptors over the above), `span`.
- **Strings**: `string`, `wstring`, `u8/u16/u32string`, `string_view`,
  `std::to_string`, `std::format` (symbol tier), `<charconv>`.
- **Memory**: `unique_ptr`, `shared_ptr`, `weak_ptr`, `make_unique`,
  `make_shared`, `enable_shared_from_this`, allocator calls collapsed.
- **Utilities**: `pair`, `tuple`, `optional`, `variant`/`visit`, `any`,
  `function`, `bitset`, `chrono` durations, `swap`/`move`/`forward` (erased).
- **Algorithms**: all of `<algorithm>` and `<numeric>`. The shapes
  `algo_recover` already detects come first (`sort`, `stable_sort`, `find`,
  `find_if`, `accumulate`, `for_each`, `transform`, `partition`,
  `lower_bound`, `upper_bound`, `binary_search`). The rest are added from
  inlined loop shapes or symbols. Comparators passed as function pointers or
  closures become lambdas.
- **Streams and I/O**: `iostream`, `fstream`, `stringstream` operator chains
  (symbol tier; their inlined form is too library-specific for tier 2).
- **Concurrency**: `thread`, `mutex`, `lock_guard`, `unique_lock`,
  `condition_variable`, `atomic` (symbol tier, plus the existing concurrency
  detections).

## The choice: user-facing surface

| Surface | Change |
|---|---|
| CLI | `parseOutputLang` accepts `cpp`/`c++`/`cxx` again; `nativeTargetHllId(Cpp)` returns `"cpp"`. New flags: `--cpp-std=c++17\|c++20\|c++23` (default `c++23`; it limits which spellings the writer uses, e.g. no `std::span` in c++17), `--cpp-stl=off\|symbols\|safe\|aggressive`, `--cpp-classes=on\|off` |
| Config | `decompParams.outputLang = "cpp"`, plus `decompParams.cpp.{std,stl,classes}` |
| GUI | Settings → Decompiler → Output language gains **C++**, with the STL level and standard. `coerceNativeOutputLang()` stops mapping `cpp` → `c`. The code view already highlights C++; the Classes panel links methods to their class |
| Buildable output | `.hpp` (classes, enums, STL includes), `_stubs.cpp`, `.buildable.cpp`; the `RETDEC_EMIT_BUILDABLE` default is unchanged |
| Managed inputs | Unchanged: input-keyed languages (Python, Java, C#, WAT) keep ignoring native `--output-lang` |

The CLI and GUI options are switched on in **phase 0**, not at the end:
phase 0's writer already produces valid C++ (C semantics in C++ spelling), so
the choice is honest from the first release that carries it.

## Phases

Each phase ends with its gates green in CI and its numbers in
[BENCHMARKS.md](../BENCHMARKS.md).

### Phase 0: a C++ writer that compiles (makes the choice real)

- `CppHLLWriter` over `CHLLWriter`:
  - Spelling and headers: `nullptr`, `bool`, `<cstdint>`, and named casts
    (`static_cast`/`reinterpret_cast`/`const_cast` chosen from the BIR cast
    kind).
  - Imports and identifiers: `extern "C"` for imported C functions; renaming
    of C++ keywords used as identifiers (`new`, `class`, `this`, `template`,
    `delete`, …).
  - Things C allows and C++ does not: no implicit `void*` conversions, no
    designated-initializer reordering, no K&R declarations, no VLAs; `goto`
    across initialisations rewritten.
- CLI, config and GUI choice wired (see above).
- **CXX-01 gate**: every corpus binary's `.cpp` passes
  `clang++ -std=c++23 -fsyntax-only`, and the buildable set links, at the same
  or better rate than CC-01. Also DET-01 determinism on `cpp`.
- Exit: CXX-01 ≥ CC-01 rate; `retdec-gui` offers C++; docs no longer say
  "rejected".

### Phase 1: classes

- Class model from `class_hierarchy` + `rtti-finder` + demangled method
  names. `thiscall` / first-argument `this` detection is checked against
  vtable slots and field accesses.
- Output: `class`/`struct` with fields from the recovered layout, methods
  defined out of line (`Foo::bar()`), base classes (single, multiple and
  virtual, with offsets checked against RTTI), and `virtual`/`override`
  markers.
- Virtual calls: `(*(*(obj))[n])(obj, …)` → `obj->method(…)`.
- Constructors and destructors: vtable stores and base-constructor chains
  become constructor bodies; member-init order is preserved.
- `new`/`delete` from `operator new`/`delete` symbols and `malloc` +
  constructor pairs; arrays with cookies.
- Namespaces from demangled names (`NamespaceGrouper`, ported).
- Tests: the ported `cxx_backend` tests, plus a new class corpus.

### Phase 2: exceptions

- Itanium: parse `.gcc_except_table` LSDAs and recognise
  `__cxa_allocate_exception`/`__cxa_throw`/`__cxa_begin_catch`/
  `__cxa_end_catch` as `throw` / `try` / `catch (T&)`; cleanup-only landing
  pads become scope ends.
- MSVC: `FuncInfo`/`TryBlockMap`/`HandlerType`, `_CxxThrowException`,
  and catch funclets.
- RAII: destructor calls on landing pads mark objects with automatic
  lifetime, so explicit destructor calls disappear.
- Gate: EH-01, try/catch/throw count against source ground truth on the EH
  corpus.

### Phase 3: STL tier 1 (symbols) and the ABI tables

- ABI tables for all three standard libraries × 32/64-bit.
- Demangled STL calls → member/free calls; template arguments → variable
  types; type propagation through the BIR (a value passed as `this` to a
  `vector<int>` member is a `vector<int>`).
- Collapse helper clusters (`_M_realloc_insert`, `__push_back_slow_path`,
  `_Emplace_reallocate`) into the operation that called them.
- **STL-01 metric**: the corpus is built with `-g`, then stripped. Ground
  truth is the STL types and calls taken from DWARF/PDB; recovery runs on the
  stripped binary. Precision and recall are reported per component and per
  standard library.

### Phase 4: STL tier 2 (structure)

- Promote `container_detect` and `algo_recover` results from labels to type
  bindings, checked against the ABI tables (offsets, roles, growth policy).
- Pattern rewrites, each with its own test and STL-01 precision gate:
  - begin/end arithmetic → `size()`/`empty()`;
  - index-with-bounds → `operator[]`/`at`;
  - growth blocks → `push_back`/`emplace_back`;
  - SSO branches → string operations;
  - tree insert/rebalance → `insert`/`emplace`;
  - hash and bucket walk → `find`/`operator[]`;
  - atomic refcount pairs → `shared_ptr` copy and destruction;
  - pointer-walk loops → range-`for`.
- `safe` thresholds are set from measured precision; nothing ships below the
  agreed precision floor.

### Phase 5: algorithms, lambdas and idioms

- All of `<algorithm>`/`<numeric>`, from inlined loop shapes and from symbols.
- Comparator and predicate functions → lambdas at the call site (captures
  inferred from the closure struct).
- `std::function` (SBO and heap forms); `auto` where the type is spelled
  elsewhere on the line; structured bindings for `pair`/`tuple` returns;
  `std::span`/`string_view` for pointer + length parameters (c++20+ only).

### Phase 6: templates (conservative)

- Group template instantiations whose bodies are identical modulo type into
  one `template<typename T>` definition, but only when every instantiation
  matches. Otherwise keep the explicit instantiations, named from the
  demangler (`Foo<int>::bar`).

### Phase 7: hardening and cleanup

- Fuzz the lowering passes (a `fuzz_cxx_lowering` target in fuzz-pr) and run
  ASan/UBSan over the C++ corpus in `sanitizers.yml`.
- Delete `src/cxx_backend/` and the unused parts of `src/codegen/`.
- Docs: user manual chapter, `SEMANTIC_OUTPUT.md` rewrite, README matrix.

## Test corpus and gates

- **`tests/cpp_corpus/`**: small C++ programs, each covering one thing
  (classes, multiple/virtual inheritance, EH, each STL component, each
  algorithm, lambdas, templates).
  - Built with GCC and Clang against libstdc++ and libc++ (Linux/macOS), and
    MSVC (Windows).
  - Each at `-O0` and `-O2`, 32- and 64-bit where the toolchain supports it.
  - Every build is kept with `-g` (ground truth) and stripped (input).
- **Gates**:
  - CXX-01: the output compiles.
  - CXX-02: on the subset with a test harness, the buildable output, compiled
    and run, matches the original.
  - EH-01: exceptions, as above.
  - STL-01: precision and recall per component and standard library, with a
    floor for `safe`.
  - DET-01: deterministic output.
  - Fuzzing and sanitizers, as in phase 7.
- **No regression to C**: CC-01 and the existing gates are unchanged. `c` is
  still the default and its output must not change byte-for-byte when `cpp`
  code lands.

## Risks

| Risk | Mitigation |
|---|---|
| Inlined STL code differs per library version and optimisation level | Versioned ABI tables, a corpus per library and level, and tier 2 rewrites gated by measured precision |
| Wrong STL claims are worse than raw C | Evidence on every rewrite, the `--cpp-stl` levels, `safe` thresholds from STL-01, and fallback to C-in-C++ |
| `this` detection errors cascade into wrong classes | Cross-check against vtable slots, RTTI offsets and field-access consistency before a function becomes a method |
| The C++ writer drifts from the C writer | `CppHLLWriter` derives from `CHLLWriter`; the shared code stays in one place, and CC-01 and CXX-01 run on every PR |
| Scope creep (templates, coroutines, modules) | Templates are phase 6 and conservative; coroutines and modules are out of scope until the rest is measured |

## Out of scope

Recovering original source formatting, comments or local variable names
beyond what debug info and the demangler give; C++20 coroutines and modules;
`<regex>` and `<locale>` internals beyond symbol-tier calls.
