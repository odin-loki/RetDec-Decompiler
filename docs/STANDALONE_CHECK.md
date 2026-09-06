# Standalone check — building and testing without LLVM

`scripts/standalone_check.sh` compiles the part of this tree that has no
third-party dependencies and runs its unit tests. It needs a C++17 compiler and
nothing else: no network, no LLVM, no CMake, no GoogleTest checkout.

```bash
./scripts/standalone_check.sh                    # everything (~1 min cold)
./scripts/standalone_check.sh algo_recover ssa   # named suites only
./scripts/standalone_check.sh --compile-only     # no tests, just compile
./scripts/standalone_check.sh --list             # what it knows about
./scripts/standalone_check.sh --audit            # re-derive the module list
./scripts/standalone_check.sh --clean            # drop the object cache
```

## Why

A normal `cmake --preset core-release` build fetches and compiles LLVM,
Capstone, Keystone, YARA and YaraMod. That is hours of CPU and tens of
gigabytes before a single assertion runs, and it is impossible in an
environment without network access to those archives.

56 of the modules under `src/` do not need any of that. The Imortek detector,
SSA, codegen, type-recovery and bytecode-parser layers are written against the
in-house `retdec/ssa` IR and the C++17 standard library only. That is where
almost all of this fork's own logic lives — and where its own regressions land.

So the fast path compiles those 56 modules, links their existing GoogleTest
suites against a shim, and runs them.

| | full CMake build | standalone check |
|---|---|---|
| Prerequisites | network, ~30 GB, CMake, Ninja | a C++17 compiler |
| Cold time | hours | ~1 minute |
| Coverage | whole product | 62 suites, ~3500 assertions over 62 modules |

Two vendored header-only dependencies are used because they are already in the
tree and cost nothing: `deps/rapidjson` (which unlocks `config`, `serdes`,
`ctypesparser` and `neural`) and `deps/whereami` (`utils`). Everything else
under `deps/` is a download stub and stays out. System zlib is linked when
present, for the suites that read JAR and APK archives.

`cli_parser` declares `cxx_std_20` in its own CMakeLists; `CXX20_MODULES` in the
script mirrors that, and `--audit` probes each module at its declared standard.
`cuda_accel` keeps its implementation in `.cu` files, which its own CMakeLists
compiles as plain C++ when CUDA is absent — `CU_AS_CXX_MODULES` does the same.

Selection is by directory, with two escapes. `EXTRA_SOURCES` names individual
files from a module that is otherwise excluded: `src/retdec/` needs LLVM in
three of its five files, but `semantic_recovery_export.cpp` — the 1773-line
emitter behind the `--buildable` sidecar — does not, and selecting by directory
would leave it untested. `PARTIAL_SUITES` does the same for a test directory
where only some files build here.

## The GoogleTest shim

`tests/standalone/gtest/gtest.h` implements the GoogleTest subset these suites
use — `TEST`, `TEST_F`, `TEST_P`, `INSTANTIATE_TEST_SUITE_P`, the
`EXPECT_`/`ASSERT_` families, `::testing::Test`, `::testing::TestWithParam`,
`--gtest_filter` — in about 600 lines with no dependencies. The suites are
**not modified**: the same source compiles against real GoogleTest under CMake
and against the shim here.

Deliberately unsupported: gmock, death tests, typed tests, XML output. Suites
that need those (`llvmir2hll`, `bin2llvmir`, …) are not in the standalone list
and keep building the normal way.

`*_FLOAT_EQ` / `*_DOUBLE_EQ` reproduce GoogleTest's 4-ULP rule rather than an
arbitrary epsilon, so moving a test between the two harnesses does not silently
change what it asserts.

## Sanitizers

`EXTRA_CXXFLAGS` reaches every compile, so the whole dependency-free layer runs
under sanitizers without a special build tree:

```bash
BUILD_DIR=build/standalone-asan \
EXTRA_CXXFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -g" \
  ./scripts/standalone_check.sh
```

A separate `BUILD_DIR` is a convenience, not a requirement. The object cache is
keyed on modification time, which cannot see a flag change, so the script stamps
the compiler and flags each build used and starts over when they differ —
otherwise a sanitized run would leave sanitized objects behind and the next
plain run would reuse them and die at the link with undefined `__asan_*`
symbols, in a directory the user did not think they had touched. Reusing one
directory therefore costs a full rebuild each time you switch; separate
directories keep both caches warm.

## Keeping the list honest

The module and suite lists in the script are hard-coded, which can rot. Two
guards:

* `--audit` re-derives which `src/` modules compile with `-Iinclude` alone and
  fails if that disagrees with the declared list in either direction — a module
  that grew a dependency, or a new dependency-free module nobody wired up. It
  checks `SUITES` the same way: a test directory whose module is already in the
  fast path but which nothing runs is a failure. And it checks the case neither
  of those can see — a module compiled on every run with no `tests/` directory
  at all, which is invisible to a check that only looks at suites that exist.
  That is how `pdbparser` came to be compiled every run, fuzzed, and fixed for
  memory safety a dozen times while nothing asserted anything about it. A module
  may be listed in `UNTESTED_MODULES` with a reason, which makes having no tests
  a decision rather than an oversight. That check found 933 test
  cases across 17 suites that were being compiled and never run.
* `.github/workflows/standalone-check.yml` runs the check under both `g++` and
  `clang++`, plus an ASan/UBSan job, on every pull request. Two compilers is not
  redundancy: Clang rejects code GCC quietly miscompiles, and the first run of
  this job found exactly that.

`EXCLUDED_SOURCES` in the script mirrors a conditional in a module's own
CMakeLists — `neural/llama_inference.cpp` is only built when llama.cpp is
present, and compiling it here would collide with `mock_inference.cpp`.
`EXCLUDED_REASONS` records modules that compile standalone but are deliberately
left out, so `--audit` can tell "nobody wired this up" from "we decided not to".

## What it has already caught

Everything below was found by the first few runs of this script, in code that
was passing CI:

| Defect | Where | Found by |
|---|---|---|
| Heap-use-after-free on a reallocated vector | `src/cfg/cfg.cpp` `resolveVirtualCalls` | ASan job |
| Iterator invalidation across a rehash | `src/cfg/cfg.cpp` `resolveJumpTables` | reading the ASan fix |
| 4096-byte read from a 3-byte buffer | `src/mini_emu/mini_emu.cpp` `mapPage` | ASan job |
| Page-length read past the end of the input image | `src/mini_emu/mini_emu.cpp` `load` | ASan job |
| `shared_ptr` cycle leaking recursive struct types | `src/ctypes/context.cpp` | LeakSanitizer |
| Spawn call able to pass the neural structural gate by trading against a comment | `src/neural/gates.cpp` | building `neural` without tree-sitter |
| `'\u2588'` in a narrow char literal: mojibake on GCC, build failure on Clang | `src/profiling/profiling.cpp` | Clang job |
| `%lld` into an `int64_t` | `src/profiling/profiling.cpp` | Clang job |
| Missing `<cassert>`, masked by GoogleTest's transitive includes | `tests/idiom_reconstruct` | shim build |
| Leaked visitor in a test fixture | `tests/ctypes` | LeakSanitizer |

## Related

`scripts/standalone_fuzz.sh` applies the same idea to the libFuzzer harnesses —
see [docs/FUZZING.md](FUZZING.md).

## Scope

This is a fast gate, not a product gate. It says nothing about lifting,
LLVM-side optimisation, the C backend, or end-to-end decompilation. Those still
need `ctest-linux` / `ctest-windows` and the benchmark jobs. What it does say —
in a minute, on any machine — is whether this fork's own analysis layer still
compiles and still passes its tests.
