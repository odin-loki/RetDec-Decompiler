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
./scripts/standalone_check.sh --update-warnings  # rewrite the warning baseline
```

## Why

A normal `cmake --preset core-release` build fetches and compiles LLVM,
Capstone, Keystone, YARA and YaraMod. That is hours of CPU and tens of
gigabytes before a single assertion runs, and it is impossible in an
environment without network access to those archives.

<!-- standalone-check: modules=62 suites=64 -->
<!-- scripts/standalone_check.sh --audit fails if the numbers above and the
     prose below disagree with the MODULES and SUITES arrays in the script. -->

62 of the modules under `src/` do not need any of that. The Imortek detector,
SSA, codegen, type-recovery and bytecode-parser layers are written against the
in-house `retdec/ssa` IR and the C++17 standard library only. That is where
almost all of this fork's own logic lives — and where its own regressions land.

So the fast path compiles those 62 modules, links their existing GoogleTest
suites against a shim, and runs them.

| | full CMake build | standalone check |
|---|---|---|
| Prerequisites | network, ~30 GB, CMake, Ninja | a C++17 compiler |
| Cold time | hours | ~1 minute |
| Coverage | whole product | 64 suites, ~4000 test cases over 62 modules, plus 493 GUI tests where Qt6 is installed and `gpu_scanner.cu`'s device half |

## The CUDA device half

`src/utils/gpu_scanner.cu` has ~660 lines behind `RETDEC_GPU_SCANNER_HOST_ONLY`
— every kernel and all the runtime plumbing — that the CMake build compiles only
when CUDA is found, so in this tree nothing compiled them. An adversarial verify
pass showed what that was worth: four fixes inside that region were reverted and
the suite stayed green.

`tests/utils/cuda_stub/` made them compile. Running them needed two more things:
the object built *without* the host-only define, passed explicitly so the linker
never pulls `gpu_scanner_cpu.o` out of `libutils.a`, and a stub that presents a
device, since `cudaGetDeviceCount` returning 0 sent every method down the CPU
fallback the `utils` suite already covers.

The assertions are `tests/utils/gpu_scanner_tests.cpp` — the same 17 the `utils`
suite runs against the CPU path, not a copy. Two implementations of one class,
one set of assertions, and they have to agree.

Measured: an off-by-one in `findAllKernel` leaves `utils` green at 351 tests and
turns this red.

What the stub does not model is documented at the top of `cuda_runtime.h`: no
warp semantics, no coalescing, blocks serialised. A kernel with an inter-block
race passes here and fails on a device.

## The GUI suite

`src/gui` is the one part of this fork that is not header-only-plus-C++17: it is
37 sources and 493 tests behind `find_package(Qt6 REQUIRED)`, and before it was
added here nothing in any fast gate compiled a line of it. Seven defects were
found there by reading alone, two of them use-after-frees.

It does not fit the loop that builds the other suites. Every `Q_OBJECT` class
needs `moc`, the resource bundle needs `rcc`, and a `QApplication` has to exist
before the first widget — so `check_gui` in the script does those three things
itself and links the result against Qt plus `retdec-neural`, which is all the
suite actually needs. `RETDEC_GUI_HEADLESS=1` and `QT_QPA_PLATFORM=offscreen`
are what the project's own ctest run sets; the first is load-bearing, because
without it the panels defer a rehighlight into an offscreen widget that has no
viewport and the run stalls.

`RETDEC_GUI_HAS_NEURAL` is defined, because a default build defines it —
compiling without it would check the `#else` branches, which is not what ships.

Where Qt6 is absent the suite skips itself, says so, and counts as neither a
pass nor a failure. Install `qt6-base-dev` and `qt6-base-dev-tools` to run it;
the CI workflow does.

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
`--gtest_filter` — in about 830 lines with no dependencies. The suites are
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

  The `SUITES` half of that used to fire only when the module under test was
  already in the fast path — which is the one case that cannot go unnoticed for
  long. Excluding a module from `MODULES` therefore took its whole test suite
  off the map for free, and `sem_decoder`'s 45 cases sat in that hole with two
  live defects in them. It now checks *every* `tests/` directory: a suite that
  is in neither `SUITES` nor `GATED_ELSEWHERE` nor `UNGATED_SUITES` fails the
  audit, and a name in either of those two lists that no longer matches a
  directory — or that `SUITES` has since started running — fails it too.

  `GATED_ELSEWHERE` names, per suite, the CI job that actually executes it.
  "It runs in ctest" is not accepted as an answer, because it does not: measured
  with a two-test probe project against CMake's `GoogleTest` module,
  `gtest_discover_tests` attaches **no labels**, so `ctest -L unit` — what
  `ctest-linux.yml` runs — selects none of the discovered cases even when the
  target is built; and `ctest-linux.yml` names its build targets explicitly, so
  no test target is compiled in the first place, leaving only an unlabelled
  `<target>_NOT_BUILT` placeholder that `-L unit` also skips. `ctest -L unit`
  reaches exactly the three `tests/` directories that set `LABELS` themselves.

  `UNGATED_SUITES` is the debt list: suites whose assertions are evaluated
  nowhere at all, each with the reason. It is not an exemption — an entry leaves
  when a gate starts running the suite. It started at 16 entries and is down to 12, of which 4 are real suites --
  `bin2llvmir`, `capstone2llvmir`, `llvmir-emul` on the pinned `llvm-project`
  build and `unpacker` on YARA -- 1 (`opencl`) needs an ICD loader, and the
  rest are fixture directories, the gtest shim itself, or behind an
  off-by-default option.

  The four that remain were each probed rather than assumed, and each reason is
  now what was measured: `bin2llvmir`, `capstone2llvmir` and `llvmir-emul` use
  LLVM 20+/21+ APIs (`CmpPredicate`, `Intrinsic::getOrInsertDeclaration`,
  `Value::hasUseList`, the four-argument `APInt` constructor) that the
  distribution LLVM does not have -- real drift across many files, not a
  missing `-I` -- and `capstone2llvmir`'s tests additionally need
  `<keystone/keystone.h>`, which `deps/keystone` is a download stub for.

  Three left because the assumption that put them there did not survive being
  checked. `demangler` (125 assertions) was listed as needing the pinned LLVM;
  `llvm/Demangle` is among the most stable corners of LLVM and DEM-01 builds it
  against the distribution one. `cpdetect` and `loader` (74) were listed as
  needing `retdec::fileformat`, which was listed as needing the pinned LLVM; it
  needs `llvm/Object/COFF.h` and a few `llvm/Support` headers, which `llvm-dev`
  provides, plus stb, tlsh and authenticode-parser, which are vendored in
  `deps/` with sources rather than being download stubs. FF-01 builds all of
  it. `unpacker` really is blocked, but by YARA rather than by LLVM, and the
  entry now says so.

  `fileformat` is the fourth, and it was hidden differently: it is *in* `SUITES`,
  which reads as covered, but its `PARTIAL_SUITES` entry names the four files
  that need no LLVM and the other twelve went nowhere. Eleven of those twelve
  also carried a reason in `check_cmake_sources.sh` — "not carried through the
  LLVM 23.1.0 migration and not yet re-enabled" — so two separate lists agreed
  they were dead. All twelve compile and pass against the distribution
  `llvm-dev`, and FF-01 has run the whole directory since. 83 of the suite's 129
  cases had been evaluated by nothing.
* `.github/workflows/standalone-check.yml` runs the check under both `g++` and
  `clang++`, plus an ASan/UBSan job, on every pull request. Two compilers is not
  redundancy: Clang rejects code GCC quietly miscompiles, and the first run of
  this job found exactly that.

## Compiler warnings

Every module is compiled with `-Wall`. For a long time the diagnostic output was
written to a per-object log and deleted on a successful compile, so 555 warnings
across 45 translation units were produced and discarded on every run. Two of
them were bugs, and neither was found by reading the warning:

* `src/java_emitter/java_stmt_emitter.cpp`, `-Wunused-result` — *ignoring return
  value of `vector::back()`, declared with attribute `nodiscard`*, on an
  expression whose `pop_back()` consumed the value of every local variable
  assignment the Java emitter produced.
* `src/codegen/emitter.cpp`, `-Wdangling-else`, three times — on the `else` that
  made a non-`Block` loop body vanish from the emitted C.

The logs are kept now, and held against `scripts/compiler_warnings_baseline.txt`
in the same shape as `scripts/check_std_includes.sh`: the list may shrink, and
adding to it needs a reason. A warning is keyed by file and flag rather than by
line, so moving code does not churn the baseline. Only files compiled in the
current run are checked — the object cache means an unchanged file produces no
log, and a missing log is not evidence either way.

The baseline is the union over the three configurations the workflow builds:
`g++`, `clang++`, and `g++` with `-fsanitize=address,undefined -O1`. Each
reports warnings the others do not, and the gate has to pass under all three, so
regenerating it means one `--update-warnings` run per configuration from a clean
`BUILD_DIR`, unioned.

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
