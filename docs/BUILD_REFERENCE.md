# RetDec build reference

This document is the **canonical guide** to configuring, compiling, installing, packaging, and validating RetDec. It complements [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md) (MSVC + Qt6; CUDA toolkit optional) and [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md) (MinGW cross-compile).

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
| **CMake** | **3.26+** to use presets | [CMakePresets.json](../CMakePresets.json) `cmakeMinimumRequired` is 3.26.0. Root [CMakeLists.txt](../CMakeLists.txt) still declares `cmake_minimum_required(VERSION 3.13)`; invoking `cmake --preset` needs 3.26 anyway. CI installer/ctest jobs pin **3.31.6**. Superbuild CMakeLists also requires 3.26. |
| **Ninja** | any | Default generator in presets. |
| **C++ compiler** | C++17 | MSVC 2019+ (Windows), GCC 11+ / Clang 14+ (Linux), Apple Clang (macOS). |
| **Python 3** | 3.4+ | LLVM TableGen and scripts. Release packaging pins 3.12 (`support/install-share.py` / tarfile PEP 706). |
| **Perl** | any | Bundled OpenSSL builds (Windows / MinGW). |
| **Qt 6** | 6.4+ | Widgets, Core, Gui, Test — **required** for `full-linux-*` and `full-windows-*` (GUI). macOS CI uses Homebrew `qt@6`. |
| **CUDA Toolkit** | 11.8+ (typical) | Optional and **opt-in** (`RETDEC_ENABLE_CUDA_ACCEL=OFF` by default, including full presets). The acceleration layer is experimental and not in the decompiler pipeline. |
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
| `dist/windows-bundle/` | NSIS/zip staging from `build-windows-installer.ps1`. |
| `dist/` | Installer output: Linux `retdec-<ver>-linux-x64.tar.gz`, Windows `*-setup.exe` / `*-portable.zip`, macOS `retdec-<ver>-macos-<arch>.tar.gz`. |
| `install/macos/` | Default `--install-dir` of `build-macos-installer.sh`. Unix CMake presets still install to `install/linux/`; CI and [releases/README.md](../releases/README.md) pass `--install-dir install/linux`. |
| `releases/linux/`, `releases/macos/` | Git-tracked `install.sh` / `uninstall.sh` only (not the tarballs). |

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
| `core-debug` | Unix (Linux/WSL/macOS) | Debug | Smaller component set; tests ON; `build/linux`. Hidden on Windows. |
| `core-debug-msvc` | Windows | Debug | Same core set as `core-debug`; `build/windows`. |
| `core-release` | Unix | Release + LTO | Smaller set; tests OFF. |
| `core-release-msvc` | Windows | Release + LTO | Same core set as `core-release`. |
| `core-asan` | Unix | Debug + ASan | AddressSanitizer (inherits `core-debug`). |
| `core-coverage` | Unix | Debug + gcov | Coverage instrumentation. |
| `full-linux-debug` | Linux/WSL/macOS | Debug | Full tree, Qt6 GUI, tests ON; CUDA accel **OFF**. |
| `full-linux-release` | Linux/WSL/macOS | Release + LTO | Full tree, Qt6 GUI, tests ON; CUDA accel **OFF**. |
| `full-windows-release` | Windows only | Release + LTO | MSVC, bundled OpenSSL, Qt6, tests ON; CUDA accel **OFF**. |
| `full-windows-debug` | Windows only | Debug | Same components as release; PDB-friendly. |

Windows `full-*` and `core-*-msvc` presets are **hidden** on non-Windows hosts (CMake preset conditions). Unix `core-*` / `full-linux-*` are hidden on Windows. Full presets stay **CPU-only** unless you pass `-DRETDEC_ENABLE_CUDA_ACCEL=ON`.

**Build presets** exist for every configure preset above (including `core-debug-msvc` / `core-release-msvc`):

```bash
cmake --build --preset full-linux-release
```

**Test presets** exist for `core-debug`, `full-linux-debug`, and `full-windows-debug`:

```bash
ctest --preset full-linux-debug
```

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

`windows_native_build.ps1` runs `cmake --build`, `cmake --install` into `install/windows`, then copies binaries, `share/retdec`, Qt DLLs (`windeployqt`), and MSVC runtimes into `dist/windows`. If a CUDA toolkit is present it also copies CUDA runtime DLLs; that does not enable `cuda_accel` in the decompiler pipeline.

For a **debuggable GUI** copy with PDBs, use `windows_prepare_debuggable_gui.ps1` (reads from `dist/windows`, writes `dist/windows/debuggable` by default).

### macOS tarball (`dist/retdec-<ver>-macos-<arch>.tar.gz`)

`scripts/build-macos-installer.sh` stages `bin/`, `lib/`, `share/`, `RetDec.app` (renamed from `retdec-gui.app`), `install.sh`, and `uninstall.sh`. The job in [release-installers.yml](../.github/workflows/release-installers.yml) (`macos-installer`) produces that tarball. Loose GitHub Release assets are named `install-macos.sh` / `uninstall-macos.sh` so they do not overwrite Linux `install.sh`. The package is **ad-hoc signed, not notarised**; `install.sh` strips `com.apple.quarantine`. [scripts/ci/check_macos_bundle.py](../scripts/ci/check_macos_bundle.py) (MAC-01) fails if rpaths still point outside the bundle.

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

- **`full-*`** presets enable the **default “everything”** product build (Qt6 GUI, bundled OpenSSL on Windows, etc.). `RETDEC_ENABLE_CUDA_ACCEL` stays **OFF**.
- **`core-*`** presets enable a **reduced** set of `RETDEC_ENABLE_*` options suitable for faster CLI-focused builds. CMake logic still pulls in libraries required by `retdec-decompiler` (e.g. unpacker, extractors) via [cmake/options.cmake](../cmake/options.cmake).

To customise components, use `-DRETDEC_ENABLE_*=ON/OFF` or `-DRETDEC_ENABLE_ALL=ON` and read `options.cmake` for dependency chains.

---

## CUDA, Qt6, and OpenSSL

- **CUDA:** `RETDEC_ENABLE_CUDA_ACCEL` defaults **OFF** in full presets (`C-CUDA-PIPE` withdrawn). Pass `-DRETDEC_ENABLE_CUDA_ACCEL=ON` only for parked research builds. `windows_native_configure.ps1 -NoCuda` skips the NVCC probe.
- **Qt6:** Full presets set `RETDEC_REQUIRE_QT6=ON`. Use `-AllowOptionalQt` on the Windows configure script or `-DRETDEC_REQUIRE_QT6=OFF` for CLI-only experiments.
- **OpenSSL:** On Windows, `RETDEC_BUNDLED_OPENSSL` is typically ON (MSVC build). MinGW cross-compiles also use bundled OpenSSL with the MinGW toolchain.

---

## Docker

Stock RetDec 5.0 compare **pulls** `remnux/retdec` via
`scripts/run_stock_retdec_docker.py` (Windows `docker.exe`). That is the
supported Docker use. Official Hub image `retdec/retdec:v5.0` does not exist.

The GHCR runtime image is [Dockerfile.runtime](../Dockerfile.runtime) packed
from a published Linux tarball (`.github/workflows/docker-from-release.yml`).
Anonymous pull still 401 until the package is public.

The in-tree [Dockerfile](../Dockerfile) is a dispatch-only LLVM rebuild:
Ubuntu 24.04, CMake 3.26+, Ninja, `core-release` preset:

```bash
docker build -t retdec:local .
```

[`.dockerignore`](../.dockerignore) excludes `build/`, `install/`, `dist/`, and VCS/editor junk from the build context. The runtime stage copies only the install tree from the builder stage.

---

## Continuous integration

GitHub Actions workflows under [.github/workflows/](../.github/workflows/):

| Workflow | Trigger (`on:`) | Purpose |
|----------|-----------------|---------|
| [ci-smoke.yml](../.github/workflows/ci-smoke.yml) | Push / PR (all branches) + manual | Python/docs/CMake checks, ship checklist, benchmark JSON (no decompiler LLVM build) |
| [ctest-linux.yml](../.github/workflows/ctest-linux.yml) | Push / PR to `main` + manual | Linux `full-linux-debug` build, headless GUI, `ctest -L unit` and `-L integration`, QUAL-01 clang-tidy (warn-only), CACHE-05 (`fib_smoke` + ci-core when present), ELF hardening |
| [ctest-macos.yml](../.github/workflows/ctest-macos.yml) | Push / PR to `main` + manual | macOS `full-linux-release` build, MAC-01 relocatable `retdec-gui.app`, `ctest` of every registered test |
| [ctest-windows.yml](../.github/workflows/ctest-windows.yml) | **Schedule** (`0 4 * * *`) + **manual only** — not on push/PR | Windows `full-windows-debug` build, headless GUI, `ctest` |
| [standalone-check.yml](../.github/workflows/standalone-check.yml) | Push to `main` / `claude/**`, PR, manual | LLVM-free compile+test (`scripts/standalone_check.sh`) |
| [release-installers.yml](../.github/workflows/release-installers.yml) | Tag `v*` + manual | GitHub Release + Linux/Windows/macOS installers |
| [macos-package.yml](../.github/workflows/macos-package.yml) | Nightly + manual | Rehearsal of the macOS installer tree (no publish) |
| [docker-from-release.yml](../.github/workflows/docker-from-release.yml) | After `release-installers` + push of this file / `Dockerfile.runtime` + manual | GHCR from Linux tarball |
| [appimage-from-release.yml](../.github/workflows/appimage-from-release.yml) | After `release-installers` + push of this file / `make-appimage.sh` + manual | AppImage from Linux tarball (also optional in `release-installers` via `APPIMAGE` input/env, default off) |
| [algorithm-recovery-nightly.yml](../.github/workflows/algorithm-recovery-nightly.yml) | Weekly + manual (`full_corpus` input default false) | Algorithm-recovery F1 |
| [perf-nightly.yml](../.github/workflows/perf-nightly.yml) | Weekly + manual | Performance trend JSON |
| [sanitizers.yml](../.github/workflows/sanitizers.yml) | Weekly + PR (src/cmake paths) + manual | ASan/UBSan |
| [doc-integrity.yml](../.github/workflows/doc-integrity.yml) | Push to `main` / `claude/**`, PR to `main`, manual | Python doc/license integrity (no decompiler build) |
| [cla.yml](../.github/workflows/cla.yml) | `pull_request_target` + issue comment | CLA-assistant (signatures on `cla-signatures`) |
| [codeql.yml](../.github/workflows/codeql.yml) | Push / PR to `main` + weekly + manual | CodeQL for Python and Actions (not C++) |
| [coverage.yml](../.github/workflows/coverage.yml) | Weekly + manual | gcov/lcov via `run_coverage.sh` (LLVM build) |
| [fuzz-pr.yml](../.github/workflows/fuzz-pr.yml) | Push to `main` / `claude/**`, PR to `main`, weekly, manual | Fuzz option smoke on PR; libFuzzer on schedule/dispatch |
| [qt-lgpl-evidence.yml](../.github/workflows/qt-lgpl-evidence.yml) | Weekly + manual | dumpbin `/dependents` Qt6Core.dll on Windows zip |
| [sign-release-sbom.yml](../.github/workflows/sign-release-sbom.yml) | After installers + push (this file) + manual | Cosign unsigned SBOM, tarball, Windows artefacts |
| [upload-sample-binary.yml](../.github/workflows/upload-sample-binary.yml) | After installers + push (this file / `fib.c`) + manual | Upload `fib_smoke` / `fib_smoke.exe` to the Release |
| [docker-publish.yml](../.github/workflows/docker-publish.yml) | Manual | GHCR from in-tree Dockerfile (LLVM rebuild) |

`ctest-linux` and `ctest-macos` run on every push and PR to `main`. `ctest-windows` does **not**; use **Actions → Run workflow** or a local `ctest`. Full presets in those jobs stay CPU-only (`-DRETDEC_ENABLE_CUDA_ACCEL=OFF`).

**GUI platform plugin:** `ctest-linux` and `ctest-windows` set `RETDEC_GUI_HEADLESS=1` and `QT_QPA_PLATFORM=offscreen`. `ctest-macos` uses `QT_QPA_PLATFORM=cocoa` because MAC-01 keeps rpaths inside the bundle and the offscreen plugin is not shipped there.

**Release installers** ([release-installers.yml](../.github/workflows/release-installers.yml)):

- **Windows** (`windows-installer`, `windows-latest`): NSIS `setup.exe` + portable zip. `RETDEC_TESTS=OFF`, `CMAKE_BUILD_PARALLEL_LEVEL=1`, EnVar NSIS plugin, sigstore cosign. The job resolves version locally (git tag / dispatch `version` input / `CMakeLists.txt`) and does **not** `needs: release`, so a queued Ubuntu runner cannot block the Windows zip. `skip_build` defaults to **false**. Do not treat uploading a local `dist/` tree as the release path; tag or dispatch the workflow and let CI build.
- **Linux** (`linux-installer`, `needs: release`): tarball; AppImage only when the `appimage` dispatch input or `APPIMAGE` env/var is on (often off).
- **macOS** (`macos-installer`, `needs: release`): `retdec-<ver>-macos-<arch>.tar.gz` as described under [Install and staging](#macos-tarball-distretdec-ver-macos-archtargz).

External regression corpora are **not** cloned in CI; run those locally if you have access to private test repos.

---

## Dependency archives (`cmake/deps.cmake`)

Capstone, googletest, Keystone, LLVM, YARA, YaraMod, support package, and optional zlib URLs are defined in [cmake/deps.cmake](../cmake/deps.cmake). They are **download locations** for CMake ExternalProject logic, not optional documentation. You can override each `*_URL` via `-D` when invoking CMake if you mirror archives internally. The LLVM pin is the `LLVM_URL` / `LLVM_ARCHIVE_SHA256` pair in that file (do not edit `deps/llvm/`).

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
./build/linux/tests/utils/tests-utils
./build/linux/tests/gui/retdec-gui-tests
```

On Windows, use `build\windows\tests\...`.

### Snapshot updates

```bash
RETDEC_UPDATE_SNAPSHOTS=1 ./build/linux/tests/<suite>/retdec-*-tests
```

### Windows decompiler fixtures

[tests/decompiler/CMakeLists.txt](../tests/decompiler/CMakeLists.txt) builds `fib_smoke` from `tests/test_binaries/fib.c`. On MSVC that is `/ENTRY:main /NODEFAULTLIB` plus `msvc_fixture_printf.c` so the PE is the user program without UCRT noise. `decompiler_cli_diagnostics` uses TIMEOUT **900** on `WIN32` (180 elsewhere).

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
| Manual ctest workflow fails | Heavy build / cache | Trigger [ctest-linux.yml](../.github/workflows/ctest-linux.yml), [ctest-macos.yml](../.github/workflows/ctest-macos.yml), or [ctest-windows.yml](../.github/workflows/ctest-windows.yml) from Actions; prefer local `ctest` for iteration. |
| macOS GUI: “Available platform plugins are: cocoa” | Offscreen plugin not in the relocatable bundle | Use `QT_QPA_PLATFORM=cocoa` (what `ctest-macos.yml` sets). |

For MinGW-specific issues, see [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md). For MSVC GUI deployment, see [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md).

---

## See also

- [README.md](../README.md) — feature overview and quick start  
- [developer_guide.md](developer_guide.md) — code style, new stages, plugins  
- [user_manual.md](user_manual.md) — GUI and settings  
- [architecture.md](architecture.md) — pipeline and libraries  
- [docs/README.md](README.md) — documentation index and diagnostic env vars  
