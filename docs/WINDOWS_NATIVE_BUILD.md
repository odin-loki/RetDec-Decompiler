# Windows Native Build — MSVC + CUDA + Qt6 GUI

This guide covers building RetDec **natively on Windows** to produce a full
deployment with:

- **CUDA GPU acceleration** (NVCC + MSVC host compiler)
- **Qt6 GUI** (`retdec-gui.exe`)
- All CLI tools (`retdec-decompiler.exe`, `retdec-unpacker.exe`, etc.)

> **Why native?** NVCC (the CUDA compiler) uses the system C++ compiler as
> its host compiler. On Windows this must be MSVC. CUDA GPU kernels cannot
> be cross-compiled from Linux. If you only need the CLI decompiler without
> GPU acceleration, use the cheaper MinGW cross-compile path described in
> [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md).

**Related docs:** [BUILD_REFERENCE.md](BUILD_REFERENCE.md) (preset matrix, `build\windows`, `install\windows`, `dist\windows`, superbuild, CI) · [docs/README.md](README.md) (full index).

---

## Prerequisites

### Quick install (PowerShell, as Administrator)

```powershell
.\scripts\Install-RetdecWindowsDeps.ps1
```

This script checks for and installs (via `winget`) everything in the table below.

### Manual / offline install

| Tool | Minimum | Where to get |
|------|---------|-------------|
| **Visual Studio Build Tools 2022** | MSVC v143, Windows 11 SDK | [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/) — select "Build Tools for Visual Studio 2022"; check: C++ Build Tools, Windows 11 SDK |
| **CUDA Toolkit** | 11.8 | [developer.nvidia.com/cuda-downloads](https://developer.nvidia.com/cuda-downloads) — pick Windows > x86_64 > Local installer |
| **Qt 6 (MSVC x64)** | 6.4 | [qt.io/download-qt-installer](https://www.qt.io/download-qt-installer) — during install select: Qt 6.7.x > MSVC 2019 64-bit *or* MSVC 2022 64-bit |
| **CMake** | **3.26+** | [cmake.org/download](https://cmake.org/download/) or `winget install Kitware.CMake` (matches root `CMakePresets.json`) |
| **Ninja** | any | `winget install Ninja-build.Ninja` |
| **Strawberry Perl** | any | [strawberryperl.com](https://strawberryperl.com) — needed for bundled OpenSSL configure |
| **Git** | any | `winget install Git.Git` |

After installing Visual Studio Build Tools, all subsequent commands must be
run from a **"Developer PowerShell for VS 2022"** or
**"x64 Native Tools Command Prompt for VS 2022"** so that `cl.exe`, `nmake`,
and the Windows SDK are on `PATH`.

---

## Build steps

### Step 1 — Set Qt6_DIR (if not auto-detected)

The configure script searches common locations (`C:\Qt\6.*\msvc*_64\...`).
If Qt is installed elsewhere, set the environment variable:

```powershell
# In Developer PowerShell for VS 2022:
$env:Qt6_DIR = "C:\Qt\6.7.3\msvc2019_64\lib\cmake\Qt6"
```

Or pass it on the command line (Step 2).

### Step 2 — Configure

```powershell
.\scripts\windows_native_configure.ps1
# Debug build (MSVC Debug CRT, full PDBs):
.\scripts\windows_native_configure.ps1 -Preset full-windows-debug
.\scripts\windows_native_build.ps1 -Preset full-windows-debug
```

Optional parameters:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-Preset` | `full-windows-release` | CMake preset; binary dir `build\<Preset>\` |
| `-QtDir` | auto-detected | Qt6 CMake config dir (**required** for `full-*` unless `-AllowOptionalQt`) |
| `-CudaPath` | `$env:CUDA_PATH` | CUDA Toolkit root |
| `-NoCuda` | off | Set `RETDEC_ENABLE_CUDA_ACCEL=OFF` (full presets default to CUDA **on** when NVCC is available) |
| `-AllowOptionalQt` | off | Pass `-DRETDEC_REQUIRE_QT6=OFF` if you cannot install Qt (CLI-only; not recommended for `full-*`) |

Example with explicit paths:

```powershell
.\scripts\windows_native_configure.ps1 `
    -QtDir "C:\Qt\6.7.3\msvc2019_64\lib\cmake\Qt6" `
    -CudaPath "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.4"
```

### Step 3 — Build and stage

```powershell
.\scripts\windows_native_build.ps1
```

This runs `cmake --build`, then:
- Calls `cmake --install` into `install\windows\` (Windows preset tree)
- Copies main executables to `dist\windows\`
- Copies `share\retdec\` ( `decompiler-config.json`, `support\`, `doc\`, etc.). If the install tree has no `share\retdec`, the script **falls back** to copying `src\retdec-decompiler\decompiler-config.json`, the repo `support\` tree, and `docs\WINDOWS_NATIVE_BUILD.md` into `dist\windows\share\retdec\` so the CLI and GUI can find data next to the staged binaries.
- Runs `windeployqt` to copy Qt6 DLLs
- Copies CUDA runtime DLLs (`cudart64_*.dll`, etc.)
- Copies MSVC runtime DLLs (`msvcp140.dll`, `vcruntime140.dll`)

Build time: **60–120 minutes** (first time, LLVM + OpenSSL from scratch).
Subsequent incremental builds take a few minutes.

### Step 4 — Test

```powershell
.\scripts\Test-RetdecWindows.ps1 -DistDir dist\windows
```

The script verifies:
1. `retdec-decompiler.exe --help`
2. Decompiles Lua, Python, Java, WebAssembly samples
3. `retdec-gui.exe` launches without crashing
4. CUDA runtime DLLs are present

---

## CMake presets (full in-tree build)

For a **full** default component set (not the smaller `core-*` presets), use:

| Preset | Host | Notes |
|--------|------|--------|
| `full-linux-debug` | Linux / WSL | Debug, tests, **`RETDEC_ENABLE_CUDA_ACCEL=ON`**, **`RETDEC_REQUIRE_QT6=ON`** |
| `full-linux-release` | Linux / WSL | Release + LTO, same |
| `full-windows-release` | Windows | Shown only on Windows; Release + LTO, bundled OpenSSL, **CUDA + Qt6 GUI required** |
| `full-windows-debug` | Windows | Same components as release, **Debug** (`/MDd`), no LTO — use for PDB debugging and `windows_native_build.ps1` → `dist\windows\` by default |

Superbuild presets (`superbuild-debug`, `superbuild-release`, `superbuild-windows-cross-mingw`, `superbuild-linux-clang`) live in [`cmake/superbuild/CMakePresets.json`](../cmake/superbuild/CMakePresets.json). Configure with `cmake -S cmake/superbuild --preset <name>` from the repo root (CMake 4.x no longer allows `sourceDir` in root `CMakePresets.json`).

Examples:

```bash
cmake --preset full-linux-release
cmake --build --preset full-linux-release
ctest --preset full-linux-debug   # after configuring with full-linux-debug
```

Point CMake at Qt6 if needed: set `Qt6_DIR` or add Qt to `CMAKE_PREFIX_PATH` so `retdec-gui` configures.

After `cmake --install`, documentation is installed under `share/retdec/doc/` (e.g. `WINDOWS_NATIVE_BUILD.md`). **Help → Documentation** in `retdec-gui` opens that file when present.

---

## What gets built

| Binary | Description |
|--------|-------------|
| `retdec-decompiler.exe` | Main CLI decompiler |
| `retdec-gui.exe` | Qt6 GUI application |
| `retdec-unpacker.exe` | Archive/packer unpacker |
| `retdec-qwen3-runner.exe` | AI inference runner (Qwen3) |

CUDA-accelerated analysis passes:
- `CUDADisassembler` — parallel x86-64 CFG disassembly
- `CUDASteensgaard` — points-to alias analysis
- `CUDATypeInferencer` — type propagation
- `CUDASemanticHasher` — mini x86-64 emulator
- `CUDAEGraphSimplifier` — E-graph equality saturation

All passes fall back to CPU threading automatically if the CUDA runtime is not
available on the target machine (or if no NVIDIA GPU is present).

---

## OpenSSL — MSVC path (VC-WIN64A)

The bundled OpenSSL build now supports MSVC via `VC-WIN64A`:

- **Platform**: `VC-WIN64A` (64-bit, MSVC target)
- **Build tool**: `nmake` (found via `find_program`)
- **Library**: `libcrypto.lib` (static, installed into `external/openssl-install/lib/`)
- **No NASM required**: `no-asm` is passed
- **No extra env vars needed**: `nmake` finds `cl.exe` from the VS environment

If `nmake` is not found, CMake will error out with a message. Ensure you are
running from a Developer Command Prompt.

---

## Qt6 GUI — windeployqt

The build script runs `windeployqt6.exe` (or `windeployqt.exe`) automatically
to copy all required Qt6 DLLs into `dist\windows\`. If `windeployqt` is
not found on `PATH`, add the Qt `bin\` directory:

```powershell
$env:PATH = "C:\Qt\6.7.3\msvc2019_64\bin;" + $env:PATH
.\scripts\windows_native_build.ps1 -SkipBuild   # re-stage only
```

Qt6 modules pulled in by the GUI build:
- `Qt6Core`, `Qt6Gui`, `Qt6Widgets`, `Qt6Network` (and platform plugin `qwindows.dll`)

---

## CUDA — driver requirements

The CUDA runtime DLLs bundled in `dist\windows\` are forward-compatible
with any NVIDIA driver released after the CUDA Toolkit version you built with.
The target machine must have an NVIDIA driver installed but does **not** need the
full CUDA Toolkit.

Minimum driver versions (CUDA Toolkit → min driver):

| CUDA 11.8 | 522.06 |
| CUDA 12.0 | 527.41 |
| CUDA 12.4 | 551.61 |

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `cl.exe not found` | Run from Developer PowerShell for VS 2022 |
| `nmake not found` | Same — `nmake` is part of the VS Build Tools |
| `Qt6Config.cmake not found` | Set `-QtDir` or `$env:Qt6_DIR` |
| `nvcc not found` / CUDA not detected | Install CUDA Toolkit; ensure `CUDA_PATH` env var is set |
| OpenSSL configure fails | Perl not on PATH — install Strawberry Perl |
| `windeployqt` not found | Add Qt `bin\` to PATH |
| `libcrypto.lib` not found after build | Check OpenSSL built correctly: `external\openssl-install\lib\libcrypto.lib` must exist |
| Long build times | Normal for first build (LLVM + OpenSSL). Use `-Jobs` to control parallelism |
| `retdec-gui.exe` crashes on startup | Qt DLLs missing — run `windeployqt dist\windows\retdec-gui.exe` manually |

---

## Comparison: build paths

| Feature | MinGW cross-compile (WSL) | Native Windows (MSVC) |
|---------|--------------------------|----------------------|
| CUDA GPU acceleration | No (CPU fallback only) | **Yes** |
| Qt6 GUI | No | **Yes** |
| Build host | Linux / WSL | Windows |
| Compiler | MinGW-w64 GCC | MSVC (cl.exe) |
| Build time | ~60–90 min (first) | ~60–120 min (first) |
| Output dir | `dist\windows\` | `dist\windows\` |
| Test script | `Test-RetdecWindows.ps1 -DistDir dist\windows` | same |

---

## Related files

- `scripts\Install-RetdecWindowsDeps.ps1` — dependency installer
- `scripts\windows_native_configure.ps1` — CMake configure
- `scripts\windows_native_build.ps1` — build + deploy
- `scripts\Test-RetdecWindows.ps1` — smoke tests (both build paths)
- `deps\openssl\CMakeLists.txt` — bundled OpenSSL (MSVC + MinGW + Linux)
- `docs\MINGW_CROSS_DEEP_DIVE.md` — alternative CLI-only cross-compile path
