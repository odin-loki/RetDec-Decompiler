# Windows Native Build — MSVC + GUI

This guide covers building RetDec **natively on Windows** to produce a full
deployment with:

- **GUI** (`retdec-gui.exe`, FuseUI on Win32 — vendored in `third_party/`, nothing to install)
- CLI tools (`retdec-decompiler.exe`, `retdec-unpacker.exe`, `retdec-fileinfo.exe`)
- Optional NVCC only if you opt into parked `RETDEC_ENABLE_CUDA_ACCEL` (defaults **OFF**; not the decompiler pipeline — `C-CUDA-PIPE` is withdrawn)

> **Why native?** The MSVC GUI build and the MSVC OpenSSL bundle live on this path.
> NVCC (the CUDA compiler) also needs MSVC as host compiler **if** you opt
> into experimental `cuda_accel`. That switch is unintegrated and off by
> default. For CLI-only without a CUDA toolkit, use the MinGW cross-compile
> path in [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md).

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
| **CUDA Toolkit** | 11.8 | **Optional.** Needed only if you pass `-DRETDEC_ENABLE_CUDA_ACCEL=ON`. Full presets leave that **OFF**. [developer.nvidia.com/cuda-downloads](https://developer.nvidia.com/cuda-downloads) |
| **CMake** | **3.26+** | [cmake.org/download](https://cmake.org/download/) or `winget install Kitware.CMake`. Root `CMakeLists.txt` still has `cmake_minimum_required(VERSION 3.13)`; presets (`CMakePresets.json`) require 3.26. |
| **Ninja** | any | `winget install Ninja-build.Ninja` (also used for bundled OpenSSL) |
| **Git** | any | `winget install Git.Git` |

After installing Visual Studio Build Tools, all subsequent commands must be
run from a **"Developer PowerShell for VS 2022"** or
**"x64 Native Tools Command Prompt for VS 2022"** so that `cl.exe`, `nmake`,
and the Windows SDK are on `PATH`.

---

## Build steps

### Step 1 — Configure

```powershell
.\scripts\windows_native_configure.ps1
# Debug build (MSVC Debug CRT, full PDBs):
.\scripts\windows_native_configure.ps1 -Preset full-windows-debug
.\scripts\windows_native_build.ps1 -Preset full-windows-debug
```

Optional parameters:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `-Preset` | `full-windows-release` | CMake preset; binary dir is always `build\windows` (not `build\<Preset>`). |
| `-CudaPath` | `$env:CUDA_PATH` | CUDA Toolkit root |
| `-NoCuda` | off | Skip the NVCC probe and pass `RETDEC_ENABLE_CUDA_ACCEL=OFF`. The `full-windows-*` presets already set that option OFF. |
| `-NoGui` | off | Pass `-DRETDEC_GUI=OFF` (CLI-only). The GUI needs nothing extra, so this is only for a smaller build. |

Example with explicit paths:

```powershell
.\scripts\windows_native_configure.ps1 `
    -CudaPath "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.4"
```

### Step 2 — Build and stage

```powershell
.\scripts\windows_native_build.ps1
```

This runs `cmake --build`, then:
- Calls `cmake --install` into `install\windows\` (Windows preset tree)
- Copies main executables to `dist\windows\`
- Copies `share\retdec\` ( `decompiler-config.json`, `support\`, `doc\`, etc.). If the install tree has no `share\retdec`, the script **falls back** to copying `src\retdec-decompiler\decompiler-config.json`, the repo `support\` tree, and `docs\WINDOWS_NATIVE_BUILD.md` into `dist\windows\share\retdec\` so the CLI and GUI can find data next to the staged binaries.
- Copies the GUI's resources (`share\retdec\gui\`: FuseUI themes, fonts, pass presets) with the rest of `share\retdec\`; there are no GUI DLLs or plugins to deploy
- Copies CUDA runtime DLLs (`cudart64_*.dll`, etc.)
- Copies MSVC runtime DLLs (`msvcp140.dll`, `vcruntime140.dll`)

Build time: **60–120 minutes** (first time, LLVM + OpenSSL from scratch).
Subsequent incremental builds take a few minutes.

### Step 3 — Test

```powershell
.\scripts\Test-RetdecWindows.ps1 -DistDir dist\windows
```

The script verifies:
1. `retdec-decompiler.exe --help`
2. Decompiles Lua, Python, Java, WebAssembly samples when present
3. `retdec-gui.exe` launches without crashing (skipped if the GUI was not staged)
4. CUDA runtime DLLs — **skipped** if `cudart64_*.dll` is absent (CPU-only / MinGW)

`fib_smoke` on MSVC is built with `/ENTRY:main /NODEFAULTLIB` plus `msvc_fixture_printf.c` (`tests/decompiler/CMakeLists.txt`). `decompiler_cli_diagnostics` uses TIMEOUT 900 on WIN32.

---

## CMake presets (full in-tree build)

For a **full** default component set (not the smaller `core-*` presets), use:

| Preset | Host | Notes |
|--------|------|--------|
| `full-linux-debug` | Linux / WSL / macOS | Debug, tests, **`RETDEC_ENABLE_CUDA_ACCEL=OFF`**, **`RETDEC_GUI=ON`** (the default) |
| `full-linux-release` | Linux / WSL / macOS | Release + LTO, same |
| `full-windows-release` | Windows | Shown only on Windows; Release + LTO, bundled OpenSSL; GUI (FuseUI) built; tests ON; `RETDEC_ENABLE_CUDA_ACCEL=OFF` |
| `full-windows-debug` | Windows | Same components as release, **Debug** (`/MDd`), no LTO — use for PDB debugging and `windows_native_build.ps1` → `dist\windows\` by default |
| `core-debug-msvc` / `core-release-msvc` | Windows | Smaller core set; `build\windows` (Unix `core-debug` / `core-release` are hidden on Windows) |

Superbuild presets (`superbuild-debug`, `superbuild-release`, `superbuild-windows-cross-mingw`, `superbuild-linux-clang`) live in [`cmake/superbuild/CMakePresets.json`](../cmake/superbuild/CMakePresets.json). Configure with `cmake -S cmake/superbuild --preset <name>` from the repo root (CMake 4.x no longer allows `sourceDir` in root `CMakePresets.json`).

Examples:

```bash
cmake --preset full-linux-release
cmake --build --preset full-linux-release
ctest --preset full-linux-debug   # after configuring with full-linux-debug
```

`retdec-gui` configures with no extra paths: FuseUI and Forge come from `third_party/`. To build and test only the GUI (no LLVM), use `cmake -S cmake/gui_standalone -B build\gui -G Ninja` (GoogleTest is fetched when none is installed); see [gui/FUSEUI_PORT.md](gui/FUSEUI_PORT.md).

After `cmake --install`, documentation is installed under `share/retdec/doc/` (e.g. `WINDOWS_NATIVE_BUILD.md`). **Help → Documentation** in `retdec-gui` opens that file when present.

---

## What gets built

| Binary | Description |
|--------|-------------|
| `retdec-decompiler.exe` | Main CLI decompiler |
| `retdec-gui.exe` | GUI application (FuseUI; resources in `share\retdec\gui\`) |
| `retdec-unpacker.exe` | Archive/packer unpacker |

CUDA-accelerated analysis kernels (`CUDADisassembler`, `CUDASteensgaard`,
`CUDATypeInferencer`, `CUDASemanticHasher`, `CUDAEGraphSimplifier`) exist
under `src/cuda_accel/` but are **experimental and unintegrated**. Nothing
in the decompiler pipeline calls them. Default builds are CPU-only
(`RETDEC_ENABLE_CUDA_ACCEL=OFF`).

---

## OpenSSL — MSVC (CMake, no Perl)

Bundled OpenSSL 3.5.8 is built with [Fsu0413/openssl-externalCMake](https://github.com/Fsu0413/openssl-externalCMake) plus the official source tarball (pins in `cmake/deps.cmake`). The nested build uses **CMake + Ninja** — no Strawberry Perl, no `perl Configure`, no NASM (`OPENSSL_ASM=OFF`).

- **Output**: static `libcrypto.lib` under `build/windows/external/openssl-install/lib/`
- **Requirement**: Ninja on `PATH` (or the top-level generator falls back to MSVC)
- Run from **Developer PowerShell for VS 2022** so `cl.exe` is available for the nested OpenSSL compile

---

## GUI — nothing to deploy

`retdec-gui.exe` is FuseUI drawing a CPU-rasterised frame into a Win32 window;
it links only `user32` and `gdi32` from the Windows SDK and the MSVC runtime.
There is no toolkit DLL, platform plugin or deploy tool. What it needs at run
time is its resource tree — FuseUI themes, fonts and LLVM pass presets —
installed by `cmake --install` to `share\retdec\gui\` and found at
`<bin>\..\share\retdec\gui`. In a build tree, a POST_BUILD step stages the
same files beside the binary.

`RETDEC_GUI_HEADLESS=1` (or `--headless`) runs it with no window at all, which
is how the headless decompile and parity tests run in CI.

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
| C++23 errors (`<expected>` / `std::format` missing) | Update to Visual Studio 2022 17.6+ (`cl` 19.36+); `scripts\doctor.ps1` checks this |
| `nvcc not found` / CUDA not detected | Install CUDA Toolkit; ensure `CUDA_PATH` env var is set |
| OpenSSL external project fails | Ensure Ninja + MSVC env; see `build/windows/external/openssl-ep/build/CMakeFiles/CMakeError.log` |
| `libcrypto.lib` not found after build | Check OpenSSL built correctly: `external\openssl-install\lib\libcrypto.lib` must exist |
| Long build times | Normal for first build (LLVM + OpenSSL). Use `-Jobs` to control parallelism |
| `retdec-gui.exe` starts unstyled or without fonts | Its resources are missing: `share\retdec\gui\` must be at `<bin>\..\share\retdec\gui` (true in `install\windows`, the zip and the installer); re-run `cmake --install` |

---

## Comparison: build paths

| Feature | MinGW cross-compile (WSL) | Native Windows (MSVC) |
|---------|--------------------------|----------------------|
| Experimental CUDA accel | No NVCC | Opt-in `RETDEC_ENABLE_CUDA_ACCEL` only; default **OFF**, unintegrated |
| GUI (`retdec-gui`) | Not built by the MinGW superbuild presets | **Yes** |
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
