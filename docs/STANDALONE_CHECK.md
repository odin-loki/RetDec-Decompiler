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
| Coverage | whole product | 30 suites, ~1700 assertions over 56 modules |

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

## Keeping the list honest

The module and suite lists in the script are hard-coded, which can rot. Two
guards:

* `--audit` re-derives which `src/` modules compile with `-Iinclude` alone and
  fails if that disagrees with the declared list in either direction — a module
  that grew a dependency, or a new dependency-free module nobody wired up.
* `.github/workflows/standalone-check.yml` runs the check under both `g++` and
  `clang++`, plus an ASan/UBSan job, on every pull request.

## Scope

This is a fast gate, not a product gate. It says nothing about lifting,
LLVM-side optimisation, the C backend, or end-to-end decompilation. Those still
need `ctest-linux` / `ctest-windows` and the benchmark jobs. What it does say —
in a minute, on any machine — is whether this fork's own analysis layer still
compiles and still passes its tests.
