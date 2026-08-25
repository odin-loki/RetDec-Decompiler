# RetDec Imortek — Specification Extraction Decompiler

**A specification-extraction tool that contains a decompiler** — algorithm
recovery, semantic export, and offline neural refinement are the product;
recovered C pseudocode is a supporting artefact, not the headline.

Built on upstream [RetDec](https://github.com/avast/retdec) v5.0 (dormant since
2022), this fork adds semantic library recovery, a Qt 6 GUI, optional offline
neural refinement (llama.cpp via `RETDEC_ENABLE_LLAMACPP`), and structured
algorithm/concurrency/serialisation detection no stock decompiler ships.

---

## Results (measured)

Stand-in corpus: 216 ELF binaries. Not the OSS-Fuzz paper set.

| Metric | This fork | Stock RetDec 5.0 |
|--------|-----------|------------------|
| Recompile, **buildable C** (`--buildable`, default on) | **216/216** | **0/216** |
| Recompile, default `.c` | 0/216 | 0/216 |
| Algorithm-recovery F1, **name-blind** | **0.056** (95% CI 0.034–0.083) | n/a (no label export) |
| Algorithm-recovery F1, name-assisted (symbolicated binaries) | 1.000 | n/a |

Name-blind is the headline. Name-assisted is a second mode on binaries that
still have symbol names; it is not a product F1. Do not advertise 1.0.

Default `.c` still does not recompile on either side. The buildable sidecar
is on by default (`--buildable` / `C-EMIT` in [docs/CLAIMS.md](docs/CLAIMS.md));
`--no-buildable` or `RETDEC_EMIT_BUILDABLE=0` turns it off.

Wall-clock figures that compare this Debug/WSL fork to stock Release-in-Docker
are **not a comparison**. Treat published ~6× ratios as unmeasured.

Numbers and methodology: [docs/BENCHMARKS_TABLE.md](docs/BENCHMARKS_TABLE.md),
[docs/BENCHMARKS.md](docs/BENCHMARKS.md). Register: [docs/CLAIMS.md](docs/CLAIMS.md).

Copyright (c) 2025-2026 Odin Loch trading as Imortek.
Dual-licensed: **AGPL-3.0+** ([LICENSE-AGPL](LICENSE-AGPL)) or a commercial
licence ([LICENSE-COMMERCIAL](LICENSE-COMMERCIAL)). Third-party notices:
[NOTICE](NOTICE). See [CONTRIBUTING.md](CONTRIBUTING.md)
and [SECURITY.md](SECURITY.md).

---

## Features

### Input formats

File formats the loaders accept:

| Format | Extensions |
|--------|-----------|
| ELF (Linux / Android) | `.elf`, `.so`, `.o` |
| PE (Windows) | `.exe`, `.dll`, `.sys` |
| Mach-O (macOS / iOS) | (no extension), `.dylib` |
| CUDA PTX | `.ptx` |
| WebAssembly | `.wasm` |
| JVM bytecode | `.class`, `.jar` |
| Android DEX | `.dex`, `.apk` |
| .NET CIL | `.dll` (managed) |
| Python bytecode | `.pyc` |
| Lua bytecode | `.luac` |

Native CPU lifting maturity (not the same as “file opens”):

| Architecture | Maturity |
|--------------|----------|
| x86, x86-64 | Production |
| ARM, Thumb, MIPS, PowerPC | Partial |
| ARM64 | Incomplete |
| SPARC, SystemZ, XCore, RISC-V | Not implemented |

Detail: [docs/ARCHITECTURE_TARGETS.md](docs/ARCHITECTURE_TARGETS.md).

### Output languages

Output is **input-keyed**, not a free-choice list of eleven languages.

| Input | What the native/managed path emits |
|-------|-------------------------------------|
| Native binaries (ELF / PE / Mach-O) | **C**. “C++” is the same C writer with a `.cpp` filename. |
| Python bytecode (`.pyc`) | Python |
| Lua bytecode (`.luac`) | Lua |
| WebAssembly (`.wasm`) | WAT |
| JVM / DEX | Java-family managed path |
| .NET CIL | C#-family managed path |

F#, VB.NET, Kotlin, and CUDA-C emitters exist in-tree and are **not** wired as
general native-pipeline targets. Do not treat them as shipped output choices.

### Semantic Recovery

- **STL containers**: `std::vector`, `std::map`, `std::unordered_map`,
  `std::list`, `std::string`, `std::shared_ptr`
- **Algorithms**: sorting (introsort, merge sort, heapsort, radix),
  binary search, BFS/DFS, graph algorithms
- **Cryptography**: AES, SHA-{1,256,512}, ChaCha20, RSA, EC primitives
- **Concurrency**: `std::thread`, pthreads, Win32 threads, OpenMP, TBB,
  atomics, spinlocks
- **CUDA host**: `cudaLaunchKernel`, memory ops, streams, events, NVCC stubs
- **Serialisation**: Protobuf, FlatBuffers, MessagePack, JSON, XML
- **C++ runtime**: vtables, RTTI, constructors/destructors, exceptions

### Offline neural refinement

Optional verified, air-gapped refinement via **llama.cpp** and GGUF models
(build with `-DRETDEC_ENABLE_LLAMACPP=ON`). Enable at runtime with
`RETDEC_NEURAL_REFINE=1` and `RETDEC_NEURAL_MODEL=/path/to/model.gguf`.
Deterministic decompiler output remains the auditable primary artefact.
Neural edits: **compile** gate is `cc`/`gcc -fsyntax-only` (C is not executed);
**structural** gate is active; **differential** gate is **not implemented**
(`RETDEC_NEURAL_DIFF_GATE` warns and skips).
See [docs/NEURAL_REFINEMENT.md](docs/NEURAL_REFINEMENT.md).

### Benchmarks

See **Results** above. Tables: [docs/BENCHMARKS_TABLE.md](docs/BENCHMARKS_TABLE.md).
Harness: [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

### Qt 6 GUI (v3)

Decompilation runs the same `retdec-decompiler` subprocess as the CLI. **Cache
reuse** on re-open. The former “~24% faster Fast decompile” figure is
**withdrawn**. Post-decompile loads `.c`, `.config.json`, `.dsm`, and `.ll`.

Shipped panels (constructed in `createPanels()`):

| Place | Panels |
|-------|--------|
| Document tabs | Decompiled C, Assembly, IR, CFG, Synced (`TriPaneCodeView`) |
| Left dock | Functions |
| Workspace (right) | Strings, Inspect, Binary Browser, Target |
| Output (bottom) | Console, Problems, History, Progress |
| Tools windows | Type Hierarchy, Call Graph, Signature Studio, Diff, **AI Assistant** |
| Chrome | Triage banner |

Settings and batch-decompile are dialogs, not docks. The AI assistant is a
Tools window (`AIAssistantPanel`); it is not a permanent bottom dock.

For CI and automated tests, use headless mode:

```bash
retdec-gui --headless-decompile /path/to/binary.elf
```

See [docs/internal/GUI_ROADMAP.md](docs/internal/GUI_ROADMAP.md). Optional neural
refine uses `RETDEC_NEURAL_REFINE` and `RETDEC_NEURAL_MODEL` — there is no
`retdec-qwen3-runner` binary and no CLI `--model` flag.

---

## Maintainer scope

**Shippable at v2.0.21** — measured results are in **Results** above. Stock
RetDec 5.0 compare uses the published `remnux/retdec` image (official
`retdec/retdec:v5.0` does not exist). CI still gates stem-era `MIN_MEAN_F1=0.95`;
that is not product quality. This fork does **not** pursue the OSS-Fuzz paper
corpus or four-compiler support regen.

- Git / GitHub CLI: **Windows PowerShell only** (not dual WSL + Windows)
- Optional CI: `gh auth login` then `.\scripts\dispatch_algorithm_recovery_nightly.ps1`
- Full honesty doc: [docs/internal/MAINTAINER_SCOPE.md](docs/internal/MAINTAINER_SCOPE.md)

### Repository layout

| Path | What |
|------|------|
| [`results/`](results/README.md) | Live baselines and current stock/F1 JSON (CI reads these) |
| [`data/`](data/README.md) | Archived JSON, logs, local dumps (not committed) |
| [`docs/`](docs/README.md) | Public technical docs |
| [`docs/internal/`](docs/internal/README.md) | Maintainer notes and historical plans |
| [`scripts/`](scripts/README.md) | Build, CI, benchmark helpers |

---

## Building

### First-time setup: fetch large data files

This repository deliberately omits a handful of large source-data files
(RetDec's runtime type info, MFC ordinals, YARA signatures, and the
50 000-entry word list used by the variable-name generator) so the git
checkout stays under a megabyte per file. Pull them once before the first
build:

```bash
# Linux / macOS / WSL
./scripts/fetch-large-files.sh
```

```powershell
# Windows
.\scripts\fetch-large-files.ps1
```

Both scripts download from the upstream RetDec mirror (`avast/retdec`).
Use `--base-url` / `-BaseUrl` to point at a private mirror, or `--force` /
`-Force` to overwrite existing copies.

The Qwen3 GGUF model is also intentionally not committed (multi-GB binary).
Place your own GGUF under `models/` and set `RETDEC_NEURAL_MODEL` when
`RETDEC_NEURAL_REFINE=1`. There is no `retdec-qwen3-runner` and no `--model`
CLI flag.

### Prerequisites

On Windows, run `.\scripts\doctor.ps1` first for a read-only check of CMake,
Qt6, NSIS, Perl, Python, git-lfs, and other common build prerequisites.

| Dependency | Version | Required for |
|------------|---------|-------------|
| CMake | **3.26+** | Required by [CMakePresets.json](CMakePresets.json); all targets |
| GCC or Clang | GCC 11+ / Clang 14+ | Linux / WSL build |
| MinGW-w64 | `g++-mingw-w64-x86-64` | Windows cross-compile |
| Qt 6 | 6.4+ (Widgets, Core, Gui, Test) | **Required** for `full-linux-*` / `full-windows-release` / `full-windows-debug` presets (`retdec-gui`) |
| Python 3 | 3.4+ | LLVM TableGen scripts |
| Perl | any | OpenSSL cross-build |
| CUDA Toolkit | 11.8+ | **Optional / opt-in** (`RETDEC_ENABLE_CUDA_ACCEL` defaults **OFF**, including full presets). Experimental `cuda_accel` layer is unintegrated — not required to evaluate or build. |
| Ninja | any | Recommended generator |

### Linux / WSL build

The normal development build targets the **Linux ELF** toolchain. CMake presets
put the build tree under **`build/linux/`** or **`build/windows/`** (from `CMakePresets.json` `base`, by host OS).
The **`full-linux-*` presets require Qt 6** (same idea as the native Windows full build). CUDA acceleration is **opt-in** (`RETDEC_ENABLE_CUDA_ACCEL=OFF` by default). Install Qt dev packages first, for example on Ubuntu:

```bash
sudo apt install qt6-base-dev qt6-base-dev-tools
```

For faster I/O you can clone the repo on a Linux native filesystem (`~/projects/retdec`)
instead of `/mnt/c/...`.

```bash
cd /path/to/retdec-master
bash scripts/wsl_configure_nosudo.sh   # cmake --preset full-linux-debug

cmake --build build/linux -j"$(nproc)"
```

For a **smaller CLI-only tree** without mandating Qt, use e.g. `cmake --preset core-debug` (or pass `-DRETDEC_REQUIRE_QT6=OFF` to override a full preset).

Or any preset from [CMakePresets.json](CMakePresets.json):

```bash
cmake --preset full-linux-release
cmake --build --preset full-linux-release
```

### Windows — full build (native MSVC + Qt6 GUI)

For a complete Windows build with the **Qt6 GUI**,
you must build natively on Windows. The CUDA Toolkit is **optional**
(`RETDEC_ENABLE_CUDA_ACCEL` defaults OFF). Experimental GPU accel is
unintegrated and is not required to evaluate the product.

**Prerequisites** (install with `scripts\Install-RetdecWindowsDeps.ps1`):
| Tool | Source |
|------|--------|
| Visual Studio Build Tools 2022 (MSVC v143 + Windows SDK) | [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/) |
| CUDA Toolkit 11.8+ (optional) | [developer.nvidia.com/cuda-downloads](https://developer.nvidia.com/cuda-downloads) — not required; `RETDEC_ENABLE_CUDA_ACCEL` defaults OFF |
| Qt 6 for Windows (MSVC 2019/2022 x64) | [qt.io/download-qt-installer](https://www.qt.io/download-qt-installer) |
| CMake **3.26+**, Ninja | via winget or [cmake.org](https://cmake.org) |
| Strawberry Perl | [strawberryperl.com](https://strawberryperl.com) (needed for bundled OpenSSL) |

**Build steps (from Developer PowerShell for VS 2022):**

```powershell
# 1. Install prerequisites (run once, as Administrator):
.\scripts\Install-RetdecWindowsDeps.ps1

# 2. Configure (auto-detects CUDA, Qt6, and MSVC):
.\scripts\windows_native_configure.ps1

# 3. Build and stage into dist\windows\:
.\scripts\windows_native_build.ps1

# 4. Package installers (zip + NSIS setup.exe → releases\windows\):
.\scripts\build-windows-installer.ps1 -SkipBuild
# or in one step after build:
.\scripts\windows_native_build.ps1 -PackageInstallers

# 5. Test:
.\scripts\Test-RetdecWindows.ps1 -DistDir dist\windows
```

The `dist\windows\` folder contains the complete deployment:
- `retdec-decompiler.exe`, `retdec-gui.exe`, `retdec-unpacker.exe`
- Qt6 DLLs (deployed by `windeployqt`)
- CUDA runtime DLLs (`cudart64_*.dll`)
- MSVC runtime DLLs (`msvcp140.dll`, `vcruntime140.dll`)

> If Qt6 is not in a standard location, pass its path explicitly:
> ```powershell
> .\scripts\windows_native_configure.ps1 -QtDir "C:\Qt\6.7.3\msvc2019_64\lib\cmake\Qt6"
> ```

### Windows cross-compilation (Linux/WSL → Windows PE via MinGW-w64)

This produces a **CLI-only** build (no Qt6 GUI, no CUDA) but requires no
Windows tools — everything runs from WSL. The toolchain is `x86_64-w64-mingw32-g++`.

**Install the toolchain (WSL Ubuntu):**

```bash
sudo apt install mingw-w64 g++-mingw-w64-x86-64 ninja-build perl make
```

**Build using the provided scripts:**

```bash
# Step 1: Native Linux build (produces llvm-tblgen under build/linux/deps/...)
bash scripts/wsl_configure_nosudo.sh
cmake --build build/linux -j"$(nproc)"

# Step 2–3: MinGW cross → build/linux/mingw-w64-release, stage dist/windows/
bash scripts/wsl_cross_configure.sh
bash scripts/wsl_cross_build.sh
```

The `dist/windows/` folder will contain:
- `retdec-decompiler.exe` — the main decompiler
- `retdec-unpacker.exe` — archive unpacker
- MinGW runtime DLLs (`libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`)

**Test on Windows (PowerShell):**

```powershell
.\dist\windows\retdec-decompiler.exe --help
.\dist\windows\retdec-decompiler.exe my_binary.luac -o output.lua
scripts\Test-RetdecWindows.ps1
```

> **Note:** CUDA acceleration is not supported in the Windows cross-compile
> (NVCC cannot cross-compile). The Windows build falls back to CPU-only
> analysis. For GPU-accelerated decompilation, use the Linux/WSL build.

See [docs/MINGW_CROSS_DEEP_DIVE.md](docs/MINGW_CROSS_DEEP_DIVE.md) for the
complete walkthrough including troubleshooting.

### Run tests

```bash
# Linux/WSL:
cmake --build build/linux -j"$(nproc)"
ctest --test-dir build/linux --output-on-failure

# Windows (PowerShell, smoke tests only):
scripts\Test-RetdecWindows.ps1
```

---

## Quick Start

### GUI (Linux/WSL)

```bash
# Launch the GUI (requires WSLg or X11 forwarding)
bash scripts/launch_gui.sh
```

### Command-line decompiler

```bash
# Decompile a native binary to C
retdec-decompiler binary.elf -o output.c

# Decompile Python bytecode
retdec-decompiler script.pyc -o output.py

# Decompile Lua bytecode (5.1, 5.2, 5.3, 5.4)
retdec-decompiler script.luac -o output.lua

# Decompile a Java .class file
retdec-decompiler Hello.class -o Hello.java

# Decompile Android DEX
retdec-decompiler classes.dex -o decompiled.java

# Decompile WebAssembly to WAT
retdec-decompiler module.wasm -o module.wat

# Decompile with offline neural refine (llama.cpp build + 9B Instruct GGUF)
export RETDEC_NEURAL_REFINE=1
export RETDEC_NEURAL_MODEL=models/Qwen3.5-9B-Q4_K_M.gguf
retdec-decompiler binary.elf -o output.c
```

### Windows (.exe build)

```powershell
# Same interface, Windows paths:
.\dist\windows\retdec-decompiler.exe binary.exe -o output.c
.\dist\windows\retdec-decompiler.exe script.luac -o output.lua
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [docs/README.md](docs/README.md) | **Documentation hub** — reading order, CI, Docker, diagnostics env vars, WSL/Windows quick refs |
| [docs/BUILD_REFERENCE.md](docs/BUILD_REFERENCE.md) | **Canonical build guide** — presets, `build/linux` vs `build/windows`, superbuild, install, testing, troubleshooting |
| [docs/user_manual.md](docs/user_manual.md) | GUI walkthrough, panels, settings, export, keyboard shortcuts |
| [docs/architecture.md](docs/architecture.md) | Pipeline stages, libraries, managed-language dispatch |
| [docs/developer_guide.md](docs/developer_guide.md) | Contributing, code style, new stages, tests, debugging, plugins |
| [docs/algorithm_reference.md](docs/algorithm_reference.md) | Mathematical descriptions of key algorithms |
| [docs/pipeline_stage_map.md](docs/pipeline_stage_map.md) | Stage names ↔ source directories |
| [docs/MINGW_CROSS_DEEP_DIVE.md](docs/MINGW_CROSS_DEEP_DIVE.md) | Linux/WSL → Windows PE (MinGW), `llvm-tblgen`, OpenSSL, staging |
| [docs/WINDOWS_NATIVE_BUILD.md](docs/WINDOWS_NATIVE_BUILD.md) | Native Windows: MSVC + CUDA + Qt6, deployment, troubleshooting |
| [docs/future_directions.md](docs/future_directions.md) | Research agenda, recovery targets, open problems |
| [scripts/README.md](scripts/README.md) | Every major `scripts/*.sh` and `*.ps1` helper |

---

## Project Structure

```
include/retdec/       Public headers (installed with the library)
src/                  Implementation files
tests/                Unit and integration tests
docs/                 Documentation
scripts/              Build, launch and test scripts
build/linux/          Default CMake binary dir on Linux/WSL/macOS (presets)
build/windows/        Default CMake binary dir on Windows (presets)
install/linux/ , install/windows/   Matching install prefixes
dist/windows/         Staged Windows PE bundles (MSVC or MinGW scripts)
dist/linux/           Reserved for Linux tarball / packaging output (optional)
cmake/toolchains/     CMake toolchain files (MinGW, etc.)
```

See [docs/architecture.md](docs/architecture.md) for the complete library
dependency graph and module layout.

---

## GPU acceleration (parked research, not a product feature)

The in-tree **CUDA** (`src/cuda_accel`) and **OpenCL** (`src/opencl`)
libraries are **experimental and unintegrated**: they are not wired into
the decompiler pipeline and are not a product feature.
`RETDEC_ENABLE_CUDA_ACCEL` defaults **OFF** (including full presets).
Evaluators do not need an NVIDIA card or OpenCL ICD.

To opt in to the experimental CUDA accel library (still not linked from
`src/retdec`):

```bash
cmake -S . -B build/linux -DRETDEC_ENABLE_CUDA_ACCEL=ON ...
```

See [docs/CUDA_CAPABILITIES.md](docs/CUDA_CAPABILITIES.md).

---

## Plugin System

Extend RetDec with shared-library plugins:

```cpp
class MyPlugin : public retdec::gui::IDecompilerPlugin {
public:
    retdec::gui::PluginMetadata metadata() const override { ... }
    void runStage(retdec::gui::PipelineContext& ctx) override {
        ctx.decompiledText.prepend("// My custom pass\n");
    }
};
RETDEC_EXPORT_PLUGIN(MyPlugin)
```

Plugin types: `IDecompilerPlugin`, `IOutputPlugin`, `IVisualisationPlugin`,
`IAnalysisPlugin`. See [docs/developer_guide.md](docs/developer_guide.md#plugin)
for the full authoring guide.

---

## Performance Profiling

```cpp
#include "retdec/profiling/profiling.h"

{
    auto g = retdec::profiling::Profiler::instance().measure("my_stage");
    runStage();
}  // records elapsed time automatically

auto report = retdec::profiling::Profiler::instance().report();
std::cout << report.toText();
report.toCsv("profile.csv");
```

---

## Testing Infrastructure

```cpp
#include "retdec/testing/test_harness.h"

// Build a valid ELF64 stub in memory
auto binary = retdec::testing::TestBinary::makeELF64({0xC3});
auto path   = binary.writeToTempFile(".elf");

// Snapshot regression testing
auto r = retdec::testing::SnapshotTester("tests/snapshots")
             .compare("my_test", decompilerOutput);
EXPECT_EQ(r.result, retdec::testing::SnapshotTester::Result::Match)
    << r.diff;
```

---

## License

Copyright (c) 2025-2026 Odin Loch trading as Imortek.

This project is **dual-licensed**:

1. **AGPL-3.0+** — use, modify, and share if you also share corresponding
   source. See [LICENSE](LICENSE) and [LICENSE-AGPL](LICENSE-AGPL).
2. **Commercial** — closed-source and OEM use. See
   [LICENSE-COMMERCIAL](LICENSE-COMMERCIAL) for terms. Contact for a quote.

Commercial enquiries: **odin.loch@outlook.com.au**

Contributions are accepted under AGPL-3.0+. Opening a pull request means
you agree to those terms.
