# RetDec — Enhanced Retargetable Decompiler

A machine-code decompiler that recovers high-level source code from compiled
binaries. Built on the original RetDec/LLVM pipeline, this version adds
semantic library recovery, a multi-language output backend, an integrated AI
assistant (Qwen3 with CUDA GPU acceleration), and a rich Qt 6 GUI.

Copyright (c) 2025-2026 Odin Loch trading as Imortek.
Dual-licensed: **AGPL-3.0+ with Imortek Section 7 additions** (free for
personal, charitable, educational, and small-entity use) **OR** a tiered
commercial licence. See [LICENSE](LICENSE) for full terms.

---

## Features

### Input Formats

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

### Output Languages

C · C++ · Python · Lua · Java · Kotlin · C# · F# · Visual Basic .NET · WebAssembly Text (WAT) · CUDA C

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

### AI Assistant

Integrated [Qwen3-Coder-30B](https://huggingface.co/Qwen/Qwen3-Coder-30B-A3B-Instruct)
inference engine (MoE, custom C++ implementation with FlashAttention-2 and
**CUDA GPU acceleration**). Load any GGUF-quantised model for interactive code
analysis. Pull the model with Ollama (`ollama pull qwen3-coder:30b-a3b-q4_K_M`),
then copy the GGUF weights into `models/` in the repo root.

### Qt 6 GUI

- Synchronised Assembly / SSA IR / Decompiled code tri-pane view
- Interactive CFG visualiser with mini-map and loop highlighting
- Type hierarchy browser and vtable viewer
- Call graph explorer with SCC super-nodes
- Function list with recovery confidence badges
- Strings and constants browser with semantic classification
- Before/after diff view (Myers algorithm)
- AI chat assistant panel with streaming output
- Settings dialog (7 tabs) + plugin system

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

The Qwen3 GGUF model used by the AI assistant is also intentionally not
committed (multi-GB binary). Place your own GGUF under `models/` if you
plan to enable the AI panel, or use the assistant in CPU-only mode with
a smaller bundled checkpoint.

### Prerequisites

| Dependency | Version | Required for |
|------------|---------|-------------|
| CMake | **3.26+** | Required by [CMakePresets.json](CMakePresets.json); all targets |
| GCC or Clang | GCC 11+ / Clang 14+ | Linux / WSL build |
| MinGW-w64 | `g++-mingw-w64-x86-64` | Windows cross-compile |
| Qt 6 | 6.4+ (Widgets, Core, Gui, Test) | **Required** for `full-linux-*` / `full-windows-release` / `full-windows-debug` presets (`retdec-gui`) |
| Python 3 | 3.4+ | LLVM TableGen scripts |
| Perl | any | OpenSSL cross-build |
| CUDA Toolkit | 11.8+ | **Default ON** for full presets (GPU analysis + Qwen3); use `-NoCuda` / `-DRETDEC_ENABLE_CUDA_ACCEL=OFF` for CPU-only |
| Ninja | any | Recommended generator |

### Linux / WSL build

The normal development build targets the **Linux ELF** toolchain. CMake presets
put the build tree under **`build/linux/`** or **`build/windows/`** (from `CMakePresets.json` `base`, by host OS).
The **`full-linux-*` presets enable CUDA acceleration and require Qt 6** (same idea as the native Windows full build). Install Qt dev packages first, for example on Ubuntu:

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

### Windows — full build (native MSVC + CUDA + Qt6 GUI)

For a complete Windows build with **CUDA GPU acceleration** and the **Qt6 GUI**,
you must build natively on Windows. NVCC cannot cross-compile GPU kernels from
Linux, so this is the only path to get all features on Windows.

**Prerequisites** (install with `scripts\Install-RetdecWindowsDeps.ps1`):
| Tool | Source |
|------|--------|
| Visual Studio Build Tools 2022 (MSVC v143 + Windows SDK) | [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/) |
| CUDA Toolkit 11.8+ | [developer.nvidia.com/cuda-downloads](https://developer.nvidia.com/cuda-downloads) |
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

# 4. Test:
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
- `retdec-qwen3-runner.exe` — AI model runner
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

# Decompile with AI-assisted naming (requires Qwen3 model)
retdec-decompiler binary.elf --model models/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf -o output.c
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
| [docs/user_manual.md](docs/user_manual.md) | GUI walkthrough, panels, settings, AI assistant, export, keyboard shortcuts |
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

## GPU Acceleration (CUDA)

All GPU-accelerated analysis passes use **CUDA** (replacing the previous
OpenCL backend). On systems without an NVIDIA GPU or without CUDA Toolkit
installed, all passes automatically fall back to multi-threaded CPU
implementations — no configuration change needed.

```bash
# Check if CUDA was detected at configure time:
grep 'cuda_accel' build/linux/cmake-configure.log
# Should show: [cuda_accel] CUDA found — GPU build
# Or:          [cuda_accel] No CUDA compiler — CPU-only build
```

To enable CUDA explicitly:

```bash
cmake -S . -B build/linux -DRETDEC_ENABLE_CUDA_ACCEL=ON ...
```

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

1. **GNU Affero General Public License, version 3 or later
   (AGPL-3.0+)**, as modified by the additional terms in Section 7 of
   the [LICENSE](LICENSE) file (attribution requirement, free-tier
   eligibility for personal / charitable / educational use and
   entities under 50,000 AUD annual revenue, mandatory open-sourcing
   of modifications); OR

2. A **tiered commercial licence** with an initial setup fee and an
   annual fee based on the licensee's revenue. The commercial licence
   waives the AGPL share-modifications requirement but requires that
   research conducted with the software still be open-sourced.

Initial fees range from **5,000 AUD** (entities with 50k–500k AUD
revenue) to **200,000,000 AUD** (entities above 100 billion AUD
revenue). Annual fees are a percentage of revenue (5 %–25 %). Volume
discounts apply for 5+ licences. See [LICENSE](LICENSE) §§ 7.1–7.7
for the full schedule and payment instructions, or read
[`modified-license.md`](modified-license.md) for a Markdown
version of the same terms.

Commercial enquiries: **odin.loch@outlook.com.au**

Contributions are accepted under the AGPL-3.0+ tier — by opening a PR
you agree to the LICENSE.
