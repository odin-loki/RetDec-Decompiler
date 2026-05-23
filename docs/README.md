# RetDec documentation (in-tree)

This directory contains **technical documentation** for building, operating, extending, and debugging RetDec. It is aimed at **developers, packagers, and power users**. End-user feature marketing lives in the top-level [README.md](../README.md); day-to-day GUI usage is in [user_manual.md](user_manual.md).

---

## Suggested reading order

1. **[BUILD_REFERENCE.md](BUILD_REFERENCE.md)** — CMake 3.26+, directory layout (`build/linux`, `build/windows`, `dist/windows`), all presets, superbuild, install, CI workflows, testing, troubleshooting. **Start here** if you are compiling the project.
2. **[WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md)** — MSVC, CUDA, Qt6, `windeployqt`, bundled OpenSSL, native Windows deployment.
3. **[MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md)** — Linux/WSL → Windows PE via MinGW (CLI-only); `llvm-tblgen`, OpenSSL, staging.
4. **[user_manual.md](user_manual.md)** — Qt GUI v3 layout, settings, shortcuts, external AI runner.
5. **[developer_guide.md](developer_guide.md)** — Repository layout, code style, new pipeline stages, tests, plugins, profiling.
6. **[architecture.md](architecture.md)** — Pipeline stages, libraries, managed-language dispatch.
7. **[pipeline_stage_map.md](pipeline_stage_map.md)** — Quick stage → source mapping.
8. **[algorithm_reference.md](algorithm_reference.md)** — Formal notes on selected algorithms.
9. **[future_directions.md](future_directions.md)** — Research directions and open problems.

---

## Document index

| Document | Audience | Contents |
|----------|----------|----------|
| [BUILD_REFERENCE.md](BUILD_REFERENCE.md) | Everyone building | Presets, paths, superbuild, Docker, CI, `deps.cmake`, test commands |
| [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md) | Windows developers | MSVC + CUDA + Qt6, scripts, troubleshooting |
| [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md) | Linux/WSL packagers | MinGW cross, tblgen, OpenSSL, `dist/windows` |
| [user_manual.md](user_manual.md) | GUI users | v3 layout, panels, settings, export, shortcuts |
| [GUI_POLISH.md](GUI_POLISH.md) | GUI contributors | Polish checklist (navigation, docks, honest settings) |
| [GUI_ROADMAP.md](GUI_ROADMAP.md) | Product / GUI | Phased GUI plan, CI verification commands |
| [ENGINEERING_ROADMAP.md](ENGINEERING_ROADMAP.md) | Maintainers | Shippable engineering tiers, backlog |
| [INSTALL_WINDOWS.md](INSTALL_WINDOWS.md) | Windows users | NSIS/portable install, PATH, smoke after install |
| [INSTALL_LINUX.md](INSTALL_LINUX.md) | Linux packagers | Tarball/deb packaging, install prefixes |
| [releases/README.md](../releases/README.md) | Release managers | Git LFS artifacts, GitHub Releases layout |
| [developer_guide.md](developer_guide.md) | Contributors | Style, tests, debugging, new stages, plugins |
| [architecture.md](architecture.md) | Contributors | Full pipeline, components, dependencies |
| [pipeline_stage_map.md](pipeline_stage_map.md) | Contributors | Stage names ↔ directories |
| [algorithm_reference.md](algorithm_reference.md) | Researchers | Math-heavy algorithm notes |
| [future_directions.md](future_directions.md) | Planners | Roadmap-style topics |
| [CUDA_CAPABILITIES.md](CUDA_CAPABILITIES.md) | GPU contributors | CUDA vs CUDA-accel flags, modules, CPU fallback |
| [SEMANTIC_OUTPUT.md](SEMANTIC_OUTPUT.md) | Output authors | C vs C++ semantics, STL recovery hints |
| [SYMBOL_SERVER.md](SYMBOL_SERVER.md) | Windows analysts | PDB / symbol-server setup for richer names |
| [RESEARCH_FRONTIERS.md](RESEARCH_FRONTIERS.md) | Researchers | Tier 7 long-horizon topics (not sprint work) |

**Scripts:** [scripts/README.md](../scripts/README.md) lists every important `scripts/*.sh` and `scripts/*.ps1` helper.

### Contributors — suggested reading order

1. [BUILD_REFERENCE.md](BUILD_REFERENCE.md) — configure, presets, `ctest`, CI workflows.
2. [developer_guide.md](developer_guide.md) — layout, style, tests, plugins.
3. [ENGINEERING_ROADMAP.md](ENGINEERING_ROADMAP.md) — what to pick up next; [GUI_ROADMAP.md](GUI_ROADMAP.md) / [GUI_POLISH.md](GUI_POLISH.md) for GUI work.
4. [architecture.md](architecture.md) + [pipeline_stage_map.md](pipeline_stage_map.md) — pipeline before touching stages.
5. [user_manual.md](user_manual.md) — expected GUI behaviour when changing `retdec-gui`.

**API (Doxygen):** Configure the `doc` / `docs` CMake target if enabled in your build; main page text is maintained under [doxygen/doxygen.h](doxygen/doxygen.h).

---

## Superbuild (short)

For an orchestrated multi-component build, use the superbuild project (presets in `cmake/superbuild/CMakePresets.json`):

```bash
cmake -S cmake/superbuild --preset superbuild-release
cmake --build build/linux/superbuild-release --parallel
```

On Windows, the same preset name resolves under `build/windows/superbuild-release`. Details: [BUILD_REFERENCE.md](BUILD_REFERENCE.md#superbuild).

---

## Continuous integration

Workflows live under [.github/workflows/](../.github/workflows/):

| Workflow | Trigger | Purpose |
|----------|---------|---------|
| [ci-smoke.yml](../.github/workflows/ci-smoke.yml) | Push/PR to `main` | Lightweight Python smoke tests |
| [ctest-windows.yml](../.github/workflows/ctest-windows.yml) | **Manual only** | Windows build + headless GUI + `ctest` |
| [ctest-linux.yml](../.github/workflows/ctest-linux.yml) | **Manual only** | Linux build + headless GUI + `ctest` |
| [perf-nightly.yml](../.github/workflows/perf-nightly.yml) | Weekly + manual | Performance benchmarks (README badge) |
| [release-installers.yml](../.github/workflows/release-installers.yml) | Tags `v*` + manual | Release installers (README badge) |

Full builds are not run on every push. Trigger ctest workflows from **Actions → Run workflow**, or run `ctest` locally. Details: [BUILD_REFERENCE.md](BUILD_REFERENCE.md#continuous-integration).

---

## Docker

```bash
docker build -t retdec:local .
```

The [Dockerfile](../Dockerfile) expects the **build context** to be your checkout (no remote clone inside the file). See [BUILD_REFERENCE.md](BUILD_REFERENCE.md#docker).

---

## Developer diagnostics (environment variables)

Set to any non-empty value except `0` unless noted. These are **verbose logging** switches; they may print large amounts of data.

| Variable | Area |
|----------|------|
| **`RETDEC_HEURISTIC_DIAG`** | Jump-table recovery — guessed defaults, table size mismatch |
| **`RETDEC_DECODER_TLS_DIAG`** | Decoder — TLS callback list (RVA/VA) in `initJumpTargetsTls()` |
| **`RETDEC_PARAM_RETURN_TRACE`** | `param_return` — stages + module summary |
| **`RETDEC_EMULATION_UNPACK_DIAG`** | Stage 3 emulation unpack — input path/size, entry_rva, failure reasons |
| **`RETDEC_BIN2LLVMIR_DIAG`** | `retdec` — `pass_ms retdec-*=` per pass; `pipeline_wall_ms` |
| **`RETDEC_LLVMIR2HLL_DIAG`** | `retdec-llvmir2hll` — IR shape + `phase_ms=*` per stage |
| **`RETDEC_DECODER_IMPORT_DIAG`** | Decoder — import table counts + delay-load markers |
| **`RETDEC_DECODER_EXPORT_DIAG`** | Decoder — export table size + push/skip summary |
| **`RETDEC_DECODER_ENTRY_DIAG`** | Decoder — configured entry point + main address |
| **`RETDEC_DECODER_IR_SHAPE_DIAG`** | Decoder — end-of-pass `module_summary` |
| **`RETDEC_FORMAT_LATTICE_DIAG`** | Format lattice dispatch + `computeFormatLatticeHints` |
| **`RETDEC_CPDETECT_STRUCTURAL_DIAG`** | `cpdetect` — structural-entropy packer hit |
| **`RETDEC_CPDETECT_CODEGEN_DIAG`** | `cpdetect` — codegen fingerprint scan |

---

## WSL: configure, build, run tests

From the repo root (prefer a **native Linux filesystem** under `~/` for I/O performance, not `/mnt/c/...` if you can avoid it):

```bash
# Full preset: Qt6 dev packages required (e.g. sudo apt install qt6-base-dev qt6-base-dev-tools).
# CUDA is requested by default; configure can succeed without NVCC (CPU fallback at runtime).
bash scripts/wsl_configure_nosudo.sh   # cmake --preset full-linux-debug → build/linux/
cmake --build build/linux -j"$(nproc)"
```

Equivalent using presets only:

```bash
cmake --preset full-linux-release
cmake --build --preset full-linux-release
```

Run the full test suite:

```bash
ctest --test-dir build/linux --output-on-failure
```

**Test binaries** live under `build/linux/tests/<component>/` (names like `retdec-*-tests`). CMake targets are typically `tests-*`. The unpacker test target requires the unpacker tool to be enabled in the configured component set.

---

## Native Windows build (MSVC + CUDA + Qt6 GUI)

Full GPU + GUI builds must run **on Windows** with MSVC. See [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md).

```powershell
.\scripts\Install-RetdecWindowsDeps.ps1
.\scripts\windows_native_configure.ps1
.\scripts\windows_native_build.ps1
.\scripts\Test-RetdecWindows.ps1 -DistDir dist\windows
```

Outputs: `build\windows`, `install\windows`, staged portable tree under `dist\windows\`. Debuggable GUI staging: `windows_prepare_debuggable_gui.ps1` → `dist\windows\debuggable\` by default.

---

## Linux / WSL → Windows PE (MinGW, CLI-only)

MSVC is **not** used on this path. Toolchains: `cmake/toolchains/windows-mingw-w64.cmake`. Full narrative: [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md).

```bash
sudo apt install mingw-w64 g++-mingw-w64-x86-64 ninja-build perl make
bash scripts/wsl_configure_nosudo.sh
cmake --build build/linux -j"$(nproc)"
bash scripts/wsl_cross_configure.sh
bash scripts/wsl_cross_build.sh
```

Artifacts: `build/linux/mingw-w64-release`, `install/linux/mingw-w64-release`, staged PE files in `dist/windows/`.

### Key CMake variables (cross)

| Variable | Meaning |
|----------|---------|
| `CMAKE_TOOLCHAIN_FILE` | `cmake/toolchains/windows-mingw-w64.cmake` |
| `RETDEC_LLVM_TABLEGEN` | Host `llvm-tblgen` (default: under `build/linux/...`) |
| `RETDEC_BUNDLED_OPENSSL` | Should be `ON` for Windows cross with `fileformat` |
| `RETDEC_TESTS` | `OFF` for cross (host gtest vs PE targets) |
| `RETDEC_ENABLE_MACHO_EXTRACTOR` | `ON` for a typical `retdec-decompiler` link set |
| `RETDEC_ENABLE_UNPACKER` / `UNPACKERTOOL` | `ON` for full CLI binary |

### MinGW pitfalls (quick)

| Symptom | Fix |
|---------|-----|
| `llvm-tblgen` missing | Finish native `build/linux` first or set `RETDEC_LLVM_TABLEGEN` |
| OpenSSL tool double-prefix | Do not mix `CC`/`AR` env with `--cross-compile-prefix` for OpenSSL |
| OpenSSL in `lib64/` | Prefer `lib` layout or symlink as documented in MinGW guide |
| Missing `retdec::macho-extractor` | Enable macho extractor in options (see `cmake/options.cmake`) |

---

## Windows Console → WSL build / tests

Use **single-quoted** `bash -lc '...'` so `$(nproc)` runs in Linux:

```powershell
wsl -e bash -lc 'cd /path/to/retdec-master && bash scripts/wsl_configure_nosudo.sh && cmake --build build/linux -j$(nproc) && ctest --test-dir build/linux --output-on-failure'
```

Replace `/path/to/retdec-master` with your clone (e.g. under `/home/you/retdec-master` or `/mnt/c/...`).
