# RetDec Developer Guide

Imortek **2.0.22**. How to contribute, extend, and debug this tree.

For **CMake presets, directory layout of `build/`, superbuild, CI secrets,
and packaging**, use [BUILD_REFERENCE.md](BUILD_REFERENCE.md) first. This
file is **code**, **tests**, and **workflow**.

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

`include/retdec/` does **not** 1:1-mirror `src/`. Tools live only under
`src/`. Tests live under `tests/` and only partially mirror library names.

```
RetDec/
├── CMakeLists.txt            cmake_minimum_required 3.13; project VERSION 2.0.22
├── CMakePresets.json         cmakeMinimumRequired 3.26; core-* and full-*
├── cmake/
│   ├── deps.cmake            LLVM 23.1.0, Capstone, llama.cpp b10451, …
│   ├── options.cmake
│   └── superbuild/           Superbuild; also requires CMake 3.26
├── vcpkg.json                Leftover; no CMake/preset/workflow reads it
├── .clang-format             Custom (4-space, column 120, SortIncludes: Never)
├── include/retdec/           Public headers for libraries
├── src/                      See directory list below
├── tests/                    Unit + integration + managed + algorithm_recovery
├── docs/
├── scripts/                  Build, CI, format, neural, WSL/Windows helpers
└── tools/dev/                Optional maintainer scripts (not used by CI)
```

### `src/` (from `src/CMakeLists.txt`)

**Core decompiler / Avast-era libs (feature-gated `cond_add_subdirectory`):**
`ar-extractor`, `bin2llvmir`, `bin2pat`, `capstone2llvmir`, `common`,
`config`, `cpdetect`, `ctypes`, `ctypesparser`, `debugformat`, `demangler`,
`fileformat`, `fileinfo`, `getsig`, `idr2pat`, `llvmir-emul`, `llvmir2hll`,
`loader`, `macho-extractor`, `pat2yara`, `patterngen`, `pdbparser`, `pelib`,
`retdec`, `retdec-decompiler`, `retdectool`, `rtti-finder`, `serdes`,
`stacofin`, `unpacker`, `unpackertool`, `utils`, `yaracpp`, optional `neural`,
optional `gui`.

**Always-added libraries (many are test-only or post-pipeline — check
`scripts/ci/check_link_graph.py` and [architecture.md](architecture.md)):**
`cuda_accel`, `packer`, `mini_emu`, `compiler_detect`, `loader_sim`,
`code_data`, `sem_decoder`, `func_boundary`, `cfg`, `debug_info`, `rtti`,
`compiler_abi`, `type_seed`, `idiom_reconstruct`, `string_detect`,
`eh_reconstruct`, `ssa`, `var_recovery`, `alias_analysis`, `type_inference`,
`cfg_structure`, `call_conv`, `dce`, `codegen`, `sort_detect`,
`container_detect`, `algo_recover`, `pattern_detect`, `crypto_detect`,
`bc_module`, `jvm_parser`, `jvm_reconstruct`, `java_emitter`,
`kotlin_emitter`, `cli_parser`, `cil_reconstruct`, `csharp_emitter`,
`pyc_parser`, `py_reconstruct`, `py_emitter`, `fsharp_emitter`,
`vbnet_emitter`, `wasm_parser`, `lua_parser`, `cxx_backend`, `dex_parser`,
`serial_detect`, `concurrency_detect`, `ptx_decompile`, `module_cluster`,
`profiling`, `testing`.

`src/opencl/` exists on disk and is **not** `add_subdirectory`’d from
`src/CMakeLists.txt`. `src/experimental/` only if
`RETDEC_ENABLE_EXPERIMENTAL_SCAFFOLD`.

### `tests/` (from `tests/CMakeLists.txt`)

Mirrors many `src/` library names, plus:

- `tests/decompiler/` — CLI / format-router tests
- `tests/retdec/` — library tests for `src/retdec`
- `tests/managed_integration/` — JVM, DEX, CIL, pyc, lua, wasm fixtures
- `tests/algorithm_recovery/` — labelled corpus (gitignored built binaries)
- `tests/decompilebench/` — recompile / syntax harness
- `tests/gui/` — only if `retdec-gui` was built
- `tests/neural/` — only if `RETDEC_ENABLE_NEURAL`
- `tests/cuda_accel/` — only if `RETDEC_HAS_CUDA`
- `tests/test_binaries/`, `tests/crash_corpus/`, `tests/bounds/`

---

## Build and Test {#build}

### Prerequisites

| Tool | Minimum | Notes |
|------|---------|-------|
| CMake | **3.13** to configure the root project; **3.26** to use `CMakePresets.json` / superbuild | Do not document 3.26 as the only possible CMake |
| GCC or Clang | GCC 11 / Clang 14 typical | Libraries `cxx_std_17`; `retdec-decompiler` is `cxx_std_20` |
| Ninja | any | Presets use the Ninja generator |
| Qt6 | 6.4-class Widgets | Required only for `retdec-gui` / `full-*` presets that pull GUI |
| CUDA Toolkit | optional | `RETDEC_ENABLE_CUDA` / `RETDEC_ENABLE_CUDA_ACCEL` default **OFF** |

### Quick build (Linux/WSL)

```bash
cmake --preset full-linux-release
cmake --build --preset full-linux-release
ctest --test-dir build/linux --output-on-failure
```

Helper: `bash scripts/wsl_configure_nosudo.sh` then
`cmake --build build/linux -j"$(nproc)"`.

**Root presets:** `core-debug`, `core-release`, `core-asan`, `core-coverage`,
`full-linux-debug`, `full-linux-release`, `full-windows-release`,
`full-windows-debug` (Windows-only), plus `*-msvc` core variants. There is
**no** preset named `asan`, `tsan`, or `debug`. See
[BUILD_REFERENCE.md](BUILD_REFERENCE.md#root-cmake-presets).

CLI-only: `core-debug` / `core-release` or `-DRETDEC_REQUIRE_QT6=OFF`.

### Windows cross-compile (from Linux/WSL)

See [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md).

### Running individual test binaries

Under the configured binary dir (`build/linux` or `build/windows`), for
example:

```bash
./build/linux/tests/concurrency_detect/retdec-concurrency-detect-tests
./build/linux/tests/ptx_decompile/retdec-ptx-decompile-tests
```

A passing library test does **not** mean the library is in `decompile()`.

### Snapshot tests

```bash
RETDEC_UPDATE_SNAPSHOTS=1 ./build/linux/tests/<suite>/retdec-*-tests
```

### Soft performance assertions

```bash
RETDEC_SOFT_PERF_ASSERT=1 ctest --test-dir build/linux
```

---

## Code Style {#style}

`.clang-format` is **custom**, not Google style (Google is 2-space; this
tree is **4-space**, `ColumnLimit: 120`, `PointerAlignment: Left`,
`SortIncludes: Never`). Include order is load-bearing in some files
(see the comment at the top of `.clang-format`).

```bash
bash scripts/check_format.sh
```

### Naming conventions

| Element | Convention | Example |
|---------|------------|---------|
| Types (class, struct, enum) | `PascalCase` | `ConcurrencyModel` |
| Functions and methods | `camelCase` | `detectMutex()` |
| Member variables | `trailingUnderscore_` | `stages_` |
| Constants | `kPascalCase` | `kParallelAnalysisMinFunctions` |
| Macros | `UPPER_SNAKE` | `RETDEC_PLUGIN_API_VERSION` |
| Files | `snake_case` | `concurrency_detect.h` |
| Namespaces | `retdec::module_name` | `retdec::profiling` |

### Other style rules

- No `using namespace std;` in headers.
- Public API: Doxygen `/** @brief ... */`.
- No raw `new`/`delete` in new code — smart pointers or containers.
- Prefer `std::string_view` for read-only string parameters where the
  surrounding code already does.
- Match the copyright header of neighbouring files.
- Comments explain *why*.

---

## Writing a New Pipeline Stage {#newstage}

A new `src/foo/` library is **not** in the decompiler until something in
`src/retdec/` or `decompiler-config.json` calls it.
`scripts/ci/check_link_graph.py` will classify it as test-only if only
`tests/foo` links it.

### Path A — LLVM pass (native pipeline)

1. Implement a `llvm::FunctionPass` / `ModulePass` under
   `src/bin2llvmir/optimizations/` (or llvmir2hll).
2. Register it with the existing pass infrastructure.
3. Append the pass name to `src/retdec-decompiler/decompiler-config.json`
   `decompParams.llvmPasses` (and any `--profile` JSON under
   `src/retdec-decompiler/profiles/`).
4. Add tests under `tests/bin2llvmir/` or `tests/llvmir2hll/`.

This is how `retdec-decoder`, `retdec-idioms`, and `retdec-llvmir2hll` run.

### Path B — Post-pipeline detector

1. Consume `retdec::ssa::SSAFunction` (see `src/container_detect/` as a
   model).
2. Link the library from `src/retdec/CMakeLists.txt`.
3. Call it from `src/retdec/retdec.cpp` / `function_analysis_cache.cpp`.
4. Export via `semantic_recovery_export.cpp` if it is a user-visible hint.
5. Tests under `tests/<name>/` **and** algorithm-recovery labels if it
   claims corpus F1.

### Path C — Standalone library (default if you only add a subdirectory)

`add_subdirectory` in `src/CMakeLists.txt` + `tests/` builds and tests the
code. It will **not** change `out.c` until Path A or B. That is the state
of `src/idiom_reconstruct`, `src/module_cluster`, `src/func_boundary`, etc.

Do **not** add GUI Analysis checkboxes and assume F5 honours them. F5
spawns `retdec-decompiler`.

### Adding a managed emitter

Native binaries still emit **C**. Extra emitters are managed-format routes.

1. Parser + emitter under `src/<name>/`.
2. Wire `detectManagedFormatFromBytes` + `decompileManaged` in
   `src/retdec-decompiler/managed_decompiler.cpp`.
3. `target_link_libraries(retdec-decompiler …)` in
   `src/retdec-decompiler/CMakeLists.txt`.
4. Tests in `tests/managed_integration/`.

`kotlin_emitter`, `fsharp_emitter`, and `vbnet_emitter` exist without
step 2–3. Do not document them as CLI targets until they are dispatched.

### Adding an architecture

1. `src/capstone2llvmir/<arch>/` translator (`createArch` must not throw).
2. ABI in `src/bin2llvmir/providers/abi/`.
3. `-a` allow-list in `retdec-decompiler.cpp`.
4. fileformat machine-type detection.
5. Tests under `tests/capstone2llvmir/` and a smoke binary.

See [ARCHITECTURE_TARGETS.md](ARCHITECTURE_TARGETS.md). SPARC/SystemZ/XCore
stubs throw; RISC-V has no directory.

---

## Writing Tests {#tests}

### Unit tests (Google Test)

Each library that has tests uses `TEST` / `TEST_F` under `tests/<name>/`.

Prefer `EXPECT_*` over `ASSERT_*` unless a failed precondition would crash.

### Building synthetic binaries

```cpp
#include "retdec/testing/test_harness.h"

TEST(MyLoader, ParsesELF64Header) {
    auto binary = retdec::testing::TestBinary::makeELF64(
        {0x55, 0x48, 0x89, 0xE5, 0xC3},
        {{"main", 0x401000, true}}
    );
    auto path = binary.writeToTempFile(".elf");
    std::filesystem::remove(path);
}
```

(`retdec-testing` is a test support library — it is supposed to be
test-only.)

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

Never delete, skip, or loosen a failing test to make a change look green.

---

## Debugging {#debug}

### Verbose CLI

`retdec-decompiler` `--silent` off (default) prints `Log::phase` lines.
Pass-level dumps: `--print-after-all` / `--print-before-all` (GUI Analysis
menu can add these to the next child process).

### AddressSanitizer

```bash
cmake --preset core-asan
cmake --build --preset core-asan
ctest --test-dir build/linux --output-on-failure   # path follows the preset
```

There is no `tsan` preset in root `CMakePresets.json`.

### GUI vs CLI

Debug the decompiler in the CLI first. The GUI is a `QProcess` wrapper
(`src/gui/decompiler_launch.cpp`). `gdb` on `retdec-gui` will not step
`decompile()` unless you attach to the child.

---

## Performance Profiling {#profiling}

```cpp
#include "retdec/profiling/profiling.h"

void MyStage::run(const Function& fn) {
    auto guard = retdec::profiling::Profiler::instance().measure("my_stage");
    // ...
}
```

`src/retdec/retdec.cpp` already scopes `pipeline.pm_run`,
`analysis.detectors`, `analysis.neural_refine`, `analysis.ocl_host`, etc.

CUDA kernel timers under `include/retdec/cuda_accel/` apply only to parked
`RETDEC_ENABLE_CUDA_ACCEL` builds — not the default pipeline.

---

## Writing a Plugin {#plugin}

GUI plugins only (`include/retdec/gui/settings/plugin_interface.h`).
API version **`"1.0"`**. They cannot insert LLVM passes.

```cpp
#include "retdec/gui/settings/plugin_interface.h"
using namespace retdec::gui;

class MyPlugin : public IDecompilerPlugin {
public:
    PluginMetadata metadata() const override {
        PluginMetadata m;
        m.id = "com.example.myplugin";
        m.name = "My Plugin";
        m.version = "1.0";
        return m;
    }
    void runStage(PipelineContext& ctx) override {
        ctx.decompiledText.prepend("// Processed by MyPlugin\n");
    }
};
RETDEC_EXPORT_PLUGIN(MyPlugin)
```

Install the `.so`/`.dll` on `AppSettings::plugins.searchPaths` or
**Settings → Plugins → Install Plugin…**.

---

## Contributing {#contributing}

1. Open an issue or discussion first for new stages.
2. Read [architecture.md](architecture.md) so you do not reimplement an
   unwired library that already exists.
3. Branch names: `feat/…`, `fix/…`, `docs/…`.
4. Commits: imperative, one logical change. One dependency pin per commit.
5. `bash scripts/check_format.sh` before you ask for review.

### Pull request checklist

- [ ] Unit tests for new code; no tests deleted or `DISABLED_`.
- [ ] `ctest` (or the relevant binary) on the files you touched.
- [ ] Public APIs have Doxygen comments.
- [ ] [architecture.md](architecture.md) / [pipeline_stage_map.md](pipeline_stage_map.md)
      updated if you **wire** a library into `decompile()` or the CLI.
- [ ] Do not edit `deps/llvm/` or drive-by bump `cmake/deps.cmake`.

---

## Research Notes {#research}

### STL / algorithm recovery

Detectors live in `src/container_detect/`, `src/algo_recover/`,
`src/sort_detect/`. They are implemented and wired as **comments/JSON**.
Name-blind F1 **0.056** is the honest headline. Prototype-quality, not a
finished product.

`src/idiom_reconstruct/` is implemented and **unwired**.

### Neural KV / MoE

There is no in-tree `Qwen3Config`. Optional KV reuse is llama.cpp
`RETDEC_NEURAL_REUSE_KV` — [NEURAL_REFINEMENT.md](NEURAL_REFINEMENT.md).

### CUDA

[CUDA_CAPABILITIES.md](CUDA_CAPABILITIES.md): distinguish GPU accel
(parked), `CudaHostRecovery` (unwired), `OclHostRecovery` (log), PTX
lifter (no CLI).
