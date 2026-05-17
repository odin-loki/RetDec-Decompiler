# RetDec Developer Guide

This guide covers everything needed to contribute to, extend, and debug RetDec.

For **CMake presets, directory layout, superbuild, CI secrets, and packaging**, use the canonical [BUILD_REFERENCE.md](BUILD_REFERENCE.md) first; this file focuses on **code**, **tests**, and **workflow** inside the tree.

---

## Table of Contents

1. [Repository Layout](#layout)
2. [Build and Test](#build)
3. [Code Style](#style)
4. [Writing a New Pipeline Stage](#newstage)
5. [Writing Tests](#tests)
6. [Debugging](#debug)
7. [Performance Profiling](#profiling)
8. [Writing a Plugin](#plugin)
9. [Contributing](#contributing)
10. [Research Notes](#research)

---

## Repository Layout {#layout}

```
retdec-master/
├── CMakeLists.txt            Root superbuild
├── CMakePresets.json         Root presets → build/linux or build/windows by host
├── cmake/superbuild/         Superbuild project + CMakePresets.json
├── vcpkg.json                Dependency manifest
├── retdec-config.cmake       CMake package config for downstream consumers
├── .clang-format             Clang-format style (Google-based, 4-space indent)
├── .env.example              Environment variable template
├── include/retdec/           All public headers (installed with the library)
│   ├── concurrency_detect/   — Concurrency and synchronisation detector
│   ├── module_cluster/       — Module clustering and CMake generation
│   ├── profiling/            — Performance profiling harness
│   ├── ptx_decompile/        — PTX parser and CUDA C lifter
│   ├── qwen3/                — AI inference engine components
│   ├── testing/              — Test harness utilities
│   └── gui/
│       ├── panels/           — Qt panel widgets
│       └── settings/         — AppSettings, PluginManager, plugin interfaces
├── src/                      Implementation files (mirrors include/ layout)
├── tests/                    Tests (mirrors src/ layout)
├── docs/                     Documentation
│   ├── architecture.md       — Full pipeline and component descriptions
│   ├── user_manual.md        — End-user guide
│   ├── developer_guide.md    — This file
│   ├── algorithm_reference.md — Mathematical algorithm descriptions
│   └── future_directions.md  — Research agenda and roadmap
├── scripts/                  Build helper scripts (WSL, Windows PowerShell)
└── tools/dev/                Optional one-off maintainer scripts (not used by CI)
```

---

## Build and Test {#build}

### Prerequisites

| Tool | Minimum Version | Notes |
|------|----------------|-------|
| CMake | 3.26 | Matches `cmakeMinimumRequired` in CMakePresets.json |
| GCC or Clang | GCC 11 / Clang 14 | C++17 required |
| Ninja | any | Recommended generator |
| Qt6 | 6.4 | Widgets, Core, Gui, Test modules |
| CUDA Toolkit | 11.8 | Optional; enables GPU acceleration |
| MinGW-w64 | `g++-mingw-w64-x86-64` | Windows cross-compile only |
| Perl + make | any | OpenSSL cross-build only |

### Quick build (Linux/WSL)

The **`full-linux-*` presets** turn on **CUDA acceleration** and **require Qt 6** for `retdec-gui`. Install Qt dev packages first (e.g. `sudo apt install qt6-base-dev qt6-base-dev-tools` on Ubuntu).

```bash
# Configure into build/linux/ (preset full-linux-debug):
bash scripts/wsl_configure_nosudo.sh
cmake --build build/linux -j"$(nproc)"
```

Or manually:

```bash
cmake --preset full-linux-release
cmake --build --preset full-linux-release
ctest --test-dir build/linux --output-on-failure
```

**Preset summary** (root `CMakePresets.json`): `core-debug`, `core-release`, `core-asan`, `core-coverage`, `full-linux-debug`, `full-linux-release`, `full-windows-release`, `full-windows-debug` (Windows-only). See [BUILD_REFERENCE.md](BUILD_REFERENCE.md#root-cmake-presets).

For **CLI-only** iteration without mandating Qt on full presets, use `core-debug` / `core-release` or `-DRETDEC_REQUIRE_QT6=OFF`.

### Windows cross-compile (from Linux/WSL)

See [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md) for full details.

```bash
# 1. Ensure native build exists for llvm-tblgen:
bash scripts/wsl_configure_nosudo.sh && cmake --build build/linux -j"$(nproc)"

# 2. Configure + build Windows target:
bash scripts/wsl_cross_configure.sh
bash scripts/wsl_cross_build.sh

# 3. Test on Windows:
#    (PowerShell)  .\scripts\Test-RetdecWindows.ps1
```

### Running individual test suites

Paths are under the configured binary directory (`build/linux` or `build/windows`):

```bash
./build/linux/tests/module_cluster/retdec-module-cluster-tests
./build/linux/tests/profiling/retdec-profiling-tests
./build/linux/tests/concurrency_detect/retdec-concurrency-detect-tests
./build/linux/tests/ptx_decompile/retdec-ptx-decompile-tests
./build/linux/tests/testing/retdec-testing-tests
./build/linux/tests/gui/retdec-gui-tests
```

On Windows, use `build\windows\tests\...`.

### Updating snapshot tests

When a stage intentionally changes its output:

```bash
RETDEC_UPDATE_SNAPSHOTS=1 ./build/linux/tests/<suite>/retdec-*-tests
```

### Soft performance assertions (noisy CI)

```bash
RETDEC_SOFT_PERF_ASSERT=1 ctest --test-dir build/linux
```

---

## Code Style {#style}

All code is formatted with `clang-format` using the project's `.clang-format`.
Run `bash scripts/check_format.sh` from the repository root before committing (requires `clang-format` on `PATH`).

### Naming conventions

| Element | Convention | Example |
|---------|-----------|---------|
| Types (class, struct, enum) | `PascalCase` | `ConcurrencyModel` |
| Functions and methods | `camelCase` | `detectMutex()` |
| Member variables | `trailingUnderscore_` | `stages_` |
| Constants | `kPascalCase` | `kFlashBR` |
| Macros | `UPPER_SNAKE` | `RETDEC_EXPORT_PLUGIN` |
| Files | `snake_case` | `concurrency_detect.h` |
| Namespaces | `retdec::module_name` | `retdec::profiling` |

### Other style rules

- No `using namespace std;` in headers.
- All public API must have Doxygen `/** @brief ... */` comments.
- No raw `new`/`delete` — use smart pointers or containers.
- Prefer `std::string_view` over `const std::string&` for read-only string
  parameters.
- All new files: copyright header matching existing files.
- Comments explain *why*, not *what*. Avoid "// increment counter".

---

## Writing a New Pipeline Stage {#newstage}

### Step 1 — Header

```cpp
// include/retdec/mystage/mystage.h
#ifndef RETDEC_MYSTAGE_MYSTAGE_H
#define RETDEC_MYSTAGE_MYSTAGE_H

#include <string>
#include <vector>

namespace retdec::mystage {

/**
 * @brief Results of the MyStage analysis pass.
 */
struct MyStageResult {
    std::vector<std::string> findings;
    double confidence = 0.0;
};

/**
 * @brief Detects X in the given SSA function.
 */
class MyStageDetector {
public:
    /**
     * @brief Run the analysis.
     * @param functionName  Name of the function being analysed.
     * @param irText        SSA IR text representation.
     */
    MyStageResult analyse(const std::string& functionName,
                           const std::string& irText);
};

} // namespace retdec::mystage
#endif
```

### Step 2 — Implementation

```cpp
// src/mystage/mystage.cpp
#include "retdec/mystage/mystage.h"
#include <algorithm>

namespace retdec::mystage {

MyStageResult MyStageDetector::analyse(const std::string& name,
                                        const std::string& ir) {
    MyStageResult result;
    // ... detection logic ...
    return result;
}

} // namespace retdec::mystage
```

### Step 3 — CMake

```cmake
# src/mystage/CMakeLists.txt
add_library(retdec-mystage
    mystage.cpp
)
target_include_directories(retdec-mystage
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)
set_target_properties(retdec-mystage PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
install(TARGETS retdec-mystage ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
```

Add to `src/CMakeLists.txt`:
```cmake
add_subdirectory(mystage)
```

### Step 4 — Tests

```cmake
# tests/mystage/CMakeLists.txt
add_executable(retdec-mystage-tests
    mystage_test.cpp
    ${PROJECT_SOURCE_DIR}/src/mystage/mystage.cpp
)
target_include_directories(retdec-mystage-tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(retdec-mystage-tests PRIVATE GTest::GTest GTest::Main)
set_target_properties(retdec-mystage-tests PROPERTIES CXX_STANDARD 17)
add_test(NAME retdec-mystage-tests COMMAND retdec-mystage-tests)
```

```cpp
// tests/mystage/mystage_test.cpp
#include "retdec/mystage/mystage.h"
#include <gtest/gtest.h>

TEST(MyStage, EmptyIRReturnsNoFindings) {
    retdec::mystage::MyStageDetector d;
    auto r = d.analyse("main", "");
    EXPECT_TRUE(r.findings.empty());
}

TEST(MyStage, DetectsKnownPattern) {
    retdec::mystage::MyStageDetector d;
    auto r = d.analyse("foo", "call pthread_mutex_lock");
    EXPECT_FALSE(r.findings.empty());
}
```

Add to `tests/CMakeLists.txt`:
```cmake
add_subdirectory(mystage)
```

### Step 5 — Register in settings and GUI

1. Add a toggle to `AnalysisSettings` in `include/retdec/gui/settings/settings.h`.
2. Add a checkbox to `SettingsDialog::buildAnalysisTab()`.
3. Consult the toggle in `AnalysisBridge` before running the stage.

---

## Writing Tests {#tests}

### Unit tests (Google Test)

Each library has its own test binary in `tests/<name>/`.  All tests use
`TEST(Suite, Name)` or `TEST_F(Fixture, Name)`.

**Prefer `EXPECT_*` over `ASSERT_*`** in most cases — `ASSERT_*` aborts the
test function immediately on failure, which can hide multiple errors.  Use
`ASSERT_*` only when subsequent lines would crash on a failed precondition.

### Building synthetic binaries

```cpp
#include "retdec/testing/test_harness.h"

TEST(MyLoader, ParsesELF64Header) {
    auto binary = retdec::testing::TestBinary::makeELF64(
        {0x55, 0x48, 0x89, 0xE5, 0xC3},  // push rbp; mov rbp,rsp; ret
        {{"main", 0x401000, true}}
    );
    auto path = binary.writeToTempFile(".elf");
    // ... pass path to loader under test ...
    std::filesystem::remove(path);
}
```

### Snapshot regression tests

```cpp
#include "retdec/testing/test_harness.h"

TEST(Emitter, OutputMatchesSnapshot) {
    retdec::testing::SnapshotTester snap("tests/snapshots");
    std::string output = runMyEmitter(testInput);
    auto r = snap.compare("my_emitter_basic", output);
    EXPECT_EQ(r.result, retdec::testing::SnapshotTester::Result::Match)
        << "Snapshot mismatch:\n" << r.diff;
}
```

Create the initial snapshot: `RETDEC_UPDATE_SNAPSHOTS=1 ./build/tests/...`

### Performance assertions

```cpp
#include "retdec/testing/test_harness.h"

TEST(Profiler, AnalysisCompletesUnder500ms) {
    auto r = retdec::testing::PerformanceAsserter::benchmark(
        [&]{ runAnalysis(testBinary); },
        10,          // iterations
        testBinarySize
    );
    EXPECT_LT(r.p99Ms, 500.0) << r.format();
}
```

### Mock pipeline

```cpp
#include "retdec/testing/test_harness.h"

TEST(Pipeline, StagesChainCorrectly) {
    retdec::testing::MockPipeline p;
    p.addStage("type_infer", [](auto s) { return s + "\n// typed"; })
     .addStage("structuring",[](auto s) { return s + "\n// structured"; });
    auto out = p.run("int x;");
    EXPECT_NE(out.find("typed"),      std::string::npos);
    EXPECT_NE(out.find("structured"), std::string::npos);
    EXPECT_EQ(p.executedStages().size(), 2u);
}
```

### Corpus tests

```cpp
#include "retdec/testing/test_harness.h"

TEST(Corpus, AllELFBinariesParse) {
    retdec::testing::CorpusRunner runner("tests/corpus");
    runner.iterate([](const retdec::testing::CorpusEntry& e) {
        EXPECT_NO_THROW({ auto r = parseFile(e.binaryPath); })
            << "Failed on: " << e.binaryPath;
        return true;
    }, ".elf");
}
```

If `tests/corpus` does not exist (CI without binary fixtures), `CorpusRunner`
returns `syntheticCorpus()` automatically.

---

## Debugging {#debug}

### Verbose pipeline logging

```cpp
AppSettings::instance().advanced.verbosity = AdvancedSettings::Verbosity::Debug;
```

Or set the environment variable `RETDEC_LOG_LEVEL=debug` before launching.

### Dump intermediate IR

```cpp
AppSettings::instance().advanced.dumpIR     = true;
AppSettings::instance().advanced.dumpSSA    = true;
AppSettings::instance().advanced.irDumpPath = "/tmp/retdec_ir";
```

After analysis, `/tmp/retdec_ir/` will contain one `.ssa` file per function.

### AddressSanitizer

```bash
cmake --preset asan
cmake --build --preset asan -j$(nproc)
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 \
    ./build-asan/tests/concurrency_detect/retdec-concurrency-detect-tests
```

### ThreadSanitizer

```bash
cmake --preset tsan
cmake --build --preset tsan -j$(nproc)
TSAN_OPTIONS=halt_on_error=0 \
    ./build-tsan/tests/profiling/retdec-profiling-tests
```

### GDB with Qt

```bash
cmake --preset debug
cmake --build --preset debug
gdb -ex "set follow-fork-mode child" ./build/src/gui/retdec-gui
```

Set breakpoints by class name: `(gdb) break AIAssistantPanel::onSendQuery`

### Valgrind (Linux)

```bash
valgrind --tool=memcheck --leak-check=full \
    ./build/tests/module_cluster/retdec-module-cluster-tests
```

---

## Performance Profiling {#profiling}

### Instrument a stage

```cpp
#include "retdec/profiling/profiling.h"

void MyStage::run(const Function& fn) {
    auto guard = retdec::profiling::Profiler::instance().measure("my_stage");
    // ... work ...
}  // guard destructor records elapsed time
```

### Record per-function timing

```cpp
#include "retdec/profiling/profiling.h"
using Clock = std::chrono::steady_clock;

for (const auto& fn : functions) {
    auto t0 = Clock::now();
    analyseFunction(fn);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - t0).count();
    retdec::profiling::Profiler::instance()
        .recordFunction(fn.name, static_cast<retdec::profiling::Nanos>(ns));
}
```

### Record CUDA kernel time

```cpp
// Using CUDAProfiler from include/retdec/cuda_accel/cuda_profiler.h:
#include "retdec/cuda_accel/cuda_profiler.h"

retdec::cuda_accel::CUDAProfiler prof;
prof.startKernel("my_kernel");

myKernel<<<grid, block>>>(args...);
cudaDeviceSynchronize();

prof.stopKernel("my_kernel");
prof.printSummary();
```

### Generate a report

```cpp
auto& prof = retdec::profiling::Profiler::instance();
prof.sampleRss();  // snapshot peak RSS
auto report = prof.report();

std::cout << report.toText();   // formatted table
report.toCsv("profile.csv");    // machine-readable
std::cout << report.toJson();   // JSON for tooling
```

### Function time histogram

```cpp
retdec::profiling::FunctionHistogram hist(
    1'000,          // 1 µs minimum
    1'000'000'000,  // 1 s maximum
    30              // buckets
);
for (const auto& s : prof.report().functionSamples)
    hist.add(s.elapsedNs);
std::cout << hist.format();
std::cout << "P50=" << hist.percentile(0.5)/1e3 << " µs\n";
std::cout << "P99=" << hist.percentile(0.99)/1e3 << " µs\n";
```

---

## Writing a Plugin {#plugin}

### Minimum decompiler plugin

```cpp
// my_plugin.cpp
#include "retdec/gui/settings/plugin_interface.h"
using namespace retdec::gui;

class MyPlugin : public IDecompilerPlugin {
public:
    PluginMetadata metadata() const override {
        PluginMetadata m;
        m.id          = "com.example.myplugin";
        m.name        = "My Plugin";
        m.version     = "1.0";
        m.description = "Prepends a comment to every decompiled function.";
        m.author      = "Your Name";
        return m;
    }
    void runStage(PipelineContext& ctx) override {
        ctx.decompiledText.prepend("// Processed by MyPlugin v1.0\n");
    }
};
RETDEC_EXPORT_PLUGIN(MyPlugin)
```

### Build

```cmake
add_library(my_plugin SHARED my_plugin.cpp)
target_include_directories(my_plugin PRIVATE /path/to/retdec/include)
target_link_libraries(my_plugin PRIVATE Qt6::Core)
set_target_properties(my_plugin PROPERTIES CXX_STANDARD 17)
```

### Install

Either copy the `.so`/`.dll` to a directory in
`AppSettings::instance().plugins.searchPaths`, or use
**Settings → Plugins → Install Plugin…** in the GUI.

### API version contract

The exported `retdec_plugin_api_version()` function must return exactly
`"1.0"` (the current `RETDEC_PLUGIN_API_VERSION`).  A mismatch causes the
plugin to be rejected at load time with a `loadError` signal.

---

## Contributing {#contributing}

### Before you start

1. Open an issue or discussion to describe what you want to add.
2. Check `docs/future_directions.md` — your idea may already be planned.
3. For new pipeline stages, read the existing detector implementations
   (`concurrency_detect`, `serial_detect`) as models.

### Branch and commit conventions

- Branch names: `feat/short-description`, `fix/issue-number`, `docs/topic`.
- Commit messages: imperative mood, present tense.
  - Good: `Add RISC-V lifting frontend`
  - Bad: `Added support for RISC-V` / `riscv stuff`
- Keep commits focused — one logical change per commit.
- Run `bash scripts/check_format.sh` before every commit.

### Pull request checklist

- [ ] All new code has unit tests.
- [ ] All tests pass: e.g. `ctest --test-dir build/linux --output-on-failure` (or your configured binary dir)
- [ ] No new format drift: `bash scripts/check_format.sh`
- [ ] New public APIs have Doxygen comments.
- [ ] `docs/architecture.md` updated if new stages or modules added.
- [ ] `docs/future_directions.md` updated if a planned item is now implemented.
- [ ] Performance-sensitive code has a profiling test or benchmark.

### Adding a new language emitter

1. Create `include/retdec/<lang>_emitter/` with type, stmt, expr, file emitters.
2. Implement in `src/<lang>_emitter/`.
3. Register in `codegen`'s language dispatch table.
4. Add to the `Output Language Targets` table in `docs/future_directions.md`.
5. Add tests including at least one snapshot test.

### Adding a new architecture

1. Create a new capstone-to-LLVMIR lifter in `src/capstone2llvmir/<arch>/`.
2. Add architecture detection in `src/fileformat/`.
3. Add calling convention descriptor in `src/call_conv/`.
4. Update the architecture table in `docs/future_directions.md`.
5. Add a corpus entry in `tests/corpus/<arch>/` with at least one binary.

---

## Research Notes {#research}

### STL semantic recovery (planned)

See `docs/future_directions.md` Part 1 for the full design.

The implementation plan:
1. **Layer 1 structural detectors**: implement one per container/algorithm.
   Start with `std::vector` (three-pointer layout + growth check) and
   binary search (midpoint arithmetic + three-way branch) as the highest
   coverage/easiest targets.
2. **Layer 2 context validator**: cross-check calling context against inferred
   container type.  Key challenge: resolving the element type from arithmetic
   stride and comparator functions.
3. **Layer 3 reconstructor**: replace low-level pointer operations with
   idiomatic STL calls in the emitted output.

Prototype location: `src/container_detect/` (stub exists; implementation pending).
Algorithm detection: `src/algo_recover/` (stub exists).

### Algorithm recognition via structural fingerprints

The core idea is a library of `AlgorithmDescriptor` structs capturing:

```cpp
struct AlgorithmDescriptor {
    std::string name;      // "introsort", "binary_search", "fft", ...
    int         loopDepth; // expected maximum loop nesting depth
    bool        recursive; // makes recursive calls?
    bool        usesAuxMemory;
    std::string indexArithmetic; // regex on loop variable patterns
    double      minConfidence;
    std::function<double(const FunctionFeatures&)> scoreFunction;
};
```

`FunctionFeatures` is extracted from the SSA IR by counting:
- Loop nesting depth
- Recursive call count
- Comparison-swap pairs
- Memory allocation calls
- Pointer arithmetic patterns

### MoE expert load balancing

The current `MoeLoadMonitor` tracks per-expert activation fractions but does
not yet feed back into routing.  A planned enhancement is auxiliary loss
during fine-tuning to encourage balanced expert usage.  For inference only
(no gradient), the monitor is diagnostic — it identifies hot experts that
might become throughput bottlenecks in multi-user serving scenarios.

### Paged KV cache performance

The current paged KV cache implementation allocates blocks of 16 tokens.
For interactive analysis sessions with many short queries, the block overhead
is minimal.  For long decompilation contexts (functions with thousands of
lines), a larger block size (64 or 128 tokens) reduces the number of block
table lookups.

This is configurable via `Qwen3Config::kv_block_size` in `qwen3_config.h`.

### CUDA portability and CPU fallback

All GPU-accelerated analysis passes are implemented with a mandatory CPU
fallback path. When the CUDA runtime is unavailable (no NVIDIA GPU, no CUDA
Toolkit, or Windows cross-compile), the pass automatically uses the
multi-threaded CPU implementation. No configuration change is needed.

The pattern in every `.cu` file:

```cpp
void CUDASomePass::run(Input& in, Output& out) {
    if (!ctx_.isAvailable()) {
        runCpu(in, out);   // always implemented
        return;
    }
    runGpu(in, out);       // CUDA path
}
```

CUDA modules live under `src/cuda_accel/` and `include/retdec/cuda_accel/`.
Each module has a corresponding Google Test suite under `tests/cuda_accel/`.
