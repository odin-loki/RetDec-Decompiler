# RetDec build reference

This document is the **canonical guide** to configuring, compiling, installing, packaging, and validating RetDec. It complements [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md) (MSVC + CUDA + Qt) and [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md) (MinGW cross-compile).

---

## Table of contents

1. [Requirements](#requirements)
2. [Directory layout](#directory-layout)
3. [Root CMake presets](#root-cmake-presets)
4. [Superbuild](#superbuild)
5. [Install and staging](#install-and-staging)
6. [Shell environment (`retdec-env.sh`)](#shell-environment-retdec-envsh)
7. [Windows PowerShell helpers](#windows-powershell-helpers)
8. [Component presets: `core-*` vs `full-*`](#component-presets-core--vs-full-)
9. [CUDA, Qt6, and OpenSSL](#cuda-qt6-and-openssl)
10. [Docker](#docker)
11. [Continuous integration](#continuous-integration)
12. [Dependency archives (`cmake/deps.cmake`)](#dependency-archives-cmakedepscmake)
13. [Testing](#testing)
14. [Troubleshooting](#troubleshooting)

---

## Requirements

| Tool | Version | Notes |
|------|---------|--------|
| **CMake** | **3.26+** | Required by [CMakePresets.json](../CMakePresets.json) (`cmakeMinimumRequired`). |
| **Ninja** | any | Default generator in presets. |
| **C++ compiler** | C++17 | MSVC 2019+ (Windows), GCC 11+ / Clang 14+ (Linux). |
| **Python 3** | 3.4+ | LLVM TableGen and scripts. |
| **Perl** | any | Bundled OpenSSL builds (Windows / MinGW). |
| **Qt 6** | 6.4+ | Widgets, Core, Gui, Test — **required** for `full-linux-*` and `full-windows-*` (GUI). |
| **CUDA Toolkit** | 11.8+ (typical) | Optional but **default ON** for full presets; CPU fallback at runtime if no GPU. |
| **MinGW-w64** | Ubuntu packages | Only for Linux/WSL → Windows PE cross-compile. |

---

## Directory layout

All paths are relative to the **repository root**.

| Path | Purpose |
|------|---------|
| `build/linux/` | Default **configure + build** tree on Linux, WSL, and macOS (root `CMakePresets.json` `base`). |
| `build/windows/` | Default **configure + build** tree on Windows (same presets; host is Windows). |
| `build/linux/<preset>/` | **Superbuild** binary dir on non-Windows hosts (e.g. `build/linux/superbuild-release`). |
| `build/windows/<preset>/` | **Superbuild** binary dir on Windows. |
| `build/linux/mingw-w64-release/` | MinGW cross-compile tree (not a CMake preset name; created by `wsl_cross_configure.sh`). |
| `install/linux/` , `install/windows/` | Default **install prefixes** matching the host OS preset layout. |
| `install/linux/mingw-w64-release/` | Install prefix for the MinGW cross build. |
| `dist/windows/` | **Staged** Windows PE layout: MSVC (`windows_native_build.ps1`) or MinGW (`wsl_cross_build.sh` / `wsl_build.sh`). |
| `dist/windows/debuggable/` | Optional debuggable GUI bundle (`windows_prepare_debuggable_gui.ps1`). |
| `dist/linux/` | Reserved for Linux tarballs or packaging output (no single mandatory script today). |

**Important:** `full-linux-debug` and `full-linux-release` both use the **same** `build/linux/` directory. Switching between them requires re-running CMake configure (they are not two side-by-side trees).

---

## Root CMake presets

Configure from the **repo root**:

```bash
cmake --preset <name>
cmake --build build/linux --parallel    # Linux/WSL/macOS
# or on Windows, binary dir is build\windows
```

| Preset | Typical host | Build type | Summary |
|--------|--------------|------------|---------|
| `core-debug` | Any | Debug | Smaller component set; tests ON; good for iteration. |
| `core-release` | Any | Release + LTO | Smaller set; tests OFF. |
| `core-asan` | Unix | Debug + ASan | AddressSanitizer. |
| `core-coverage` | Unix | Debug + gcov | Coverage instrumentation. |
| `full-linux-debug` | Linux/WSL/macOS | Debug | Full tree, CUDA, Qt6 GUI, tests ON. |
| `full-linux-release` | Linux/WSL/macOS | Release + LTO | Full tree, CUDA, Qt6 GUI, tests ON. |
| `full-windows-release` | Windows only | Release + LTO | MSVC, bundled OpenSSL, CUDA, Qt6. |
| `full-windows-debug` | Windows only | Debug | Same components as release; PDB-friendly. |

**Build presets** (same names) invoke the matching configure preset:

```bash
cmake --build --preset full-linux-release
```

**Test presets** exist for `core-debug`, `full-linux-debug`, and `full-windows-debug`:

```bash
ctest --preset full-linux-debug
```

Windows full presets are **hidden** on non-Windows hosts (CMake preset conditions).

---

## Superbuild

The **superbuild** is a separate CMake project under `cmake/superbuild/`. It orchestrates ExternalProject-style builds (core, optional Qt GUI, etc.). Presets live in [cmake/superbuild/CMakePresets.json](../cmake/superbuild/CMakePresets.json).

**Output layout (by host):**

| Host | Superbuild binary dir pattern | Matching install prefix pattern |
|------|------------------------------|----------------------------------|
| **Windows** (native MSVC) | `build/windows/<preset>/` | `install/windows/<preset>/` |
| **Linux / WSL / macOS** | `build/linux/<preset>/` | `install/linux/<preset>/` |

So you get **one tree under `build/windows/`** on Windows and **one under `build/linux/`** on Unix-like hosts; preset name is the subdirectory (e.g. `superbuild-debug`).

### Configure and build (manual)

```bash
cmake -S cmake/superbuild --preset superbuild-release
cmake --build build/linux/superbuild-release --parallel   # Linux/WSL/macOS
```

On Windows, use `build\windows\superbuild-release` instead.

### Build all (Debug + Release) — helpers

| Script | When | What it builds |
|--------|------|----------------|
| [scripts/superbuild-build-all-windows.ps1](../scripts/superbuild-build-all-windows.ps1) | **Windows**, MSVC dev environment | `superbuild-debug` and `superbuild-release` under `build/windows/` |
| [scripts/superbuild-build-all-linux.sh](../scripts/superbuild-build-all-linux.sh) | **Linux / WSL** | Same two presets under `build/linux/`; optional `SUPERBUILD_MINGW=1` adds MinGW cross Debug+Release; `SUPERBUILD_CLANG=1` adds `superbuild-linux-clang` |

Examples:

```powershell
# Windows (Developer PowerShell), from repo root:
.\scripts\superbuild-build-all-windows.ps1
.\scripts\superbuild-build-all-windows.ps1 -Install
```

```bash
# WSL / Linux, from repo root:
bash scripts/superbuild-build-all-linux.sh
bash scripts/superbuild-build-all-linux.sh --install
SUPERBUILD_MINGW=1 bash scripts/superbuild-build-all-linux.sh
```

### Superbuild configure presets

- `superbuild-debug` — Debug, tests ON (where applicable), Qt GUI ON.
- `superbuild-release` — Release + LTO, tests OFF.
- `superbuild-windows-cross-mingw` — MinGW cross **Release** + LTO (Unix host only).
- `superbuild-windows-cross-mingw-debug` — MinGW cross **Debug** (Unix host only).
- `superbuild-linux-clang` — Linux native with Clang toolchain file (Release + LTO).

---

## Install and staging

### Install from a configured tree

Use the **same** binary directory you built:

```bash
cmake --install build/linux --prefix /usr/local
# default prefix from preset is install/linux — often:
cmake --install build/linux
```

On Windows (PowerShell), after a full native build:

```powershell
cmake --install build\windows
```

### Windows portable folder (`dist/windows`)

`windows_native_build.ps1` runs `cmake --build`, `cmake --install` into `install/windows`, then copies binaries, `share/retdec`, Qt DLLs (`windeployqt`), and CUDA/MSVC runtimes into `dist/windows`.

For a **debuggable GUI** copy with PDBs, use `windows_prepare_debuggable_gui.ps1` (reads from `dist/windows`, writes `dist/windows/debuggable` by default).

---

## Shell environment (`retdec-env.sh`)

Source [scripts/lib/retdec-env.sh](../scripts/lib/retdec-env.sh) from bash scripts:

```bash
source scripts/lib/retdec-env.sh
```

Exported variables (defaults shown):

| Variable | Default meaning |
|----------|-----------------|
| `RETDEC_ROOT` | Auto: parent of `scripts/lib`. |
| `RETDEC_CMAKE_PRESET_LINUX_DEBUG` | `full-linux-debug` |
| `RETDEC_CMAKE_PRESET_LINUX_REL` | `full-linux-release` |
| `RETDEC_CMAKE_PRESET_WINDOWS_REL` | `full-windows-release` |
| `RETDEC_BUILD_DEBUG` | `$RETDEC_ROOT/build/linux` |
| `RETDEC_BUILD_RELEASE` | `$RETDEC_ROOT/build/linux` |
| `RETDEC_BUILD_MINGW` | `$RETDEC_ROOT/build/linux/mingw-w64-release` |
| `RETDEC_INSTALL_MINGW` | `$RETDEC_ROOT/install/linux/mingw-w64-release` |

Override any preset name **before** sourcing if you use non-default presets.

---

## Windows PowerShell helpers

Dot-source [scripts/retdec-paths.ps1](../scripts/retdec-paths.ps1):

- `Get-RetDecRepoRoot`
- `Get-RetDecBuildDir` / `Get-RetDecInstallDir` — resolve to `build\windows` and `install\windows` (preset name kept for API compatibility only).

Main workflows:

| Script | Role |
|--------|------|
| `windows_native_configure.ps1` | vcvars + `cmake --preset` → `build\windows` |
| `windows_native_build.ps1` | build, install, stage `dist\windows` |
| `build-install-run-windows.ps1` | configure/build/install/run runner or GUI |
| `Test-RetdecWindows.ps1` | Smoke tests against `dist\windows` |

See [scripts/README.md](../scripts/README.md) for the full table.

---

## Component presets: `core-*` vs `full-*`

- **`full-*`** presets enable the **default “everything”** product build (CUDA, Qt6 GUI, bundled OpenSSL on Windows, etc.).
- **`core-*`** presets enable a **reduced** set of `RETDEC_ENABLE_*` options suitable for faster CLI-focused builds. CMake logic still pulls in libraries required by `retdec-decompiler` (e.g. unpacker, extractors) via [cmake/options.cmake](../cmake/options.cmake).

To customise components, use `-DRETDEC_ENABLE_*=ON/OFF` or `-DRETDEC_ENABLE_ALL=ON` and read `options.cmake` for dependency chains.

---

## CUDA, Qt6, and OpenSSL

- **CUDA:** `RETDEC_ENABLE_CUDA_ACCEL` defaults ON in full presets. Pass `-DRETDEC_ENABLE_CUDA_ACCEL=OFF` or use `windows_native_configure.ps1 -NoCuda` on Windows if no toolkit is installed.
- **Qt6:** Full presets set `RETDEC_REQUIRE_QT6=ON`. Use `-AllowOptionalQt` on the Windows configure script or `-DRETDEC_REQUIRE_QT6=OFF` for CLI-only experiments.
- **OpenSSL:** On Windows, `RETDEC_BUNDLED_OPENSSL` is typically ON (MSVC build). MinGW cross-compiles also use bundled OpenSSL with the MinGW toolchain.

---

## Docker

The [Dockerfile](../Dockerfile) builds from a **local checkout** on **Ubuntu 24.04 (noble)** with **CMake 3.26+**, **Ninja**, and the **`core-release`** preset (output under `build/linux/`, install prefix overridden for the image):

```bash
docker build -t retdec:local .
```

[`.dockerignore`](../.dockerignore) excludes `build/`, `install/`, `dist/`, and VCS/editor junk from the build context. The runtime stage copies only the install tree from the builder stage.

---

## Continuous integration

GitHub Actions workflows under [.github/workflows/](../.github/workflows/):

| Workflow | Trigger | Purpose |
|----------|---------|---------|
| [ci-smoke.yml](../.github/workflows/ci-smoke.yml) | Push/PR to `main` | Lightweight Python smoke tests (CLI helpers, semantic/pipeline validation) |
| [ctest-windows.yml](../.github/workflows/ctest-windows.yml) | **Manual only** (`workflow_dispatch`) | Full Windows build, headless GUI tests, `ctest -L unit` and integration tests |
| [ctest-linux.yml](../.github/workflows/ctest-linux.yml) | **Manual only** (`workflow_dispatch`) | Full Linux build, headless GUI tests, `ctest -L unit` and integration tests |
| [perf-nightly.yml](../.github/workflows/perf-nightly.yml) | Weekly schedule + manual | Performance benchmarks (README badge) |
| [release-installers.yml](../.github/workflows/release-installers.yml) | Tag push `v*` + manual | Build and publish release installers (README badge) |

Full RetDec builds are too heavy to run on every push. Use **Actions → Run workflow** for `ctest-windows` or `ctest-linux`, or run `ctest` locally (see [Testing](#testing) below).

Both ctest workflows set `RETDEC_GUI_HEADLESS=1` and `QT_QPA_PLATFORM=offscreen` for headless GUI tests. External regression corpora are **not** cloned in CI; run those locally if you have access to private test repos.

---

## Dependency archives (`cmake/deps.cmake`)

Capstone, googletest, Keystone, LLVM, YARA, YaraMod, support package, and optional zlib URLs are defined in [cmake/deps.cmake](../cmake/deps.cmake). They are **download locations** for CMake ExternalProject logic, not optional documentation. You can override each `*_URL` via `-D` when invoking CMake if you mirror archives internally.

---

## Testing

### CTest (full preset tree)

```bash
cmake --build build/linux -j"$(nproc)"
ctest --test-dir build/linux --output-on-failure
```

### Individual binaries

Test executables live under the build tree, e.g.:

```bash
./build/linux/tests/utils/retdec-utils-tests
./build/linux/tests/gui/retdec-gui-tests
```

On Windows, use `build\windows\tests\...`.

### Snapshot updates

```bash
RETDEC_UPDATE_SNAPSHOTS=1 ./build/linux/tests/<suite>/retdec-*-tests
```

### Windows smoke tests

```powershell
.\scripts\Test-RetdecWindows.ps1 -DistDir dist\windows
```

---

## Troubleshooting

| Symptom | Likely cause | What to try |
|---------|----------------|-------------|
| `retdec::ar-extractor` / similar not found | Partial `RETDEC_ENABLE_*` without dependencies | Use `full-*`, `RETDEC_ENABLE_ALL=ON`, or a recent `options.cmake` that ties `retdec-decompiler` deps. |
| `llvm-tblgen` missing (MinGW cross) | No native Linux build | Run `wsl_configure_nosudo.sh` and build `build/linux` first; or set `RETDEC_LLVM_TABLEGEN`. |
| Qt not found | Qt6 not installed or `Qt6_DIR` wrong | Install Qt MSVC x64; pass `-QtDir` to `windows_native_configure.ps1`. |
| OpenSSL / nmake failures on Windows | Not in VS environment | Use Developer PowerShell or the provided configure script (vcvars). |
| CUDA not detected | `CUDA_PATH` / toolkit missing | Install CUDA; or disable `RETDEC_ENABLE_CUDA_ACCEL`. |
| GitHub download fails during LLVM fetch | Network / HTTP2 | Retry build; clear `build/.../external/src/*-stamp/*-download` if needed. |
| Manual ctest workflow fails | Heavy build / cache | Trigger [ctest-linux.yml](../.github/workflows/ctest-linux.yml) or [ctest-windows.yml](../.github/workflows/ctest-windows.yml) from Actions; prefer local `ctest` for iteration. |

For MinGW-specific issues, see [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md). For MSVC GUI deployment, see [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md).

---

## See also

- [README.md](../README.md) — feature overview and quick start  
- [developer_guide.md](developer_guide.md) — code style, new stages, plugins  
- [user_manual.md](user_manual.md) — GUI and settings  
- [architecture.md](architecture.md) — pipeline and libraries  
- [docs/README.md](README.md) — documentation index and diagnostic env vars  
