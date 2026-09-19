# RetDec documentation (in-tree)

This directory contains **technical documentation** for building, operating, extending, and debugging RetDec Imortek **2.0.24**. It is aimed at **developers, packagers, and power users**. End-user feature marketing lives in the top-level [README.md](../README.md); day-to-day GUI usage is in [user_manual.md](user_manual.md).

`CMakeLists.txt` requires **CMake 3.13**. [CMakePresets.json](../CMakePresets.json) requires **CMake 3.26**. CUDA acceleration (`RETDEC_ENABLE_CUDA_ACCEL`) defaults **OFF** and is not wired into the decompiler.

---

## Suggested reading order

1. **[../QUICKSTART.md](../QUICKSTART.md)** — ten-minute decompile from GitHub Release assets (Docker when published, or a local binary).
2. **[BUILD_REFERENCE.md](BUILD_REFERENCE.md)** — CMake 3.13 vs presets 3.26, directory layout (`build/linux`, `build/windows`, `dist/windows`), all presets, superbuild, install, CI workflows, testing, troubleshooting. **Start here** if you are compiling the project.
3. **[INSTALL_LINUX.md](INSTALL_LINUX.md)** / **[INSTALL_WINDOWS.md](INSTALL_WINDOWS.md)** — unpacking the published tarball / NSIS / zip.
4. **[WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md)** — MSVC, Qt6, `windeployqt`, bundled OpenSSL (CUDA toolkit optional, accel unintegrated).
5. **[MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md)** — Linux/WSL → Windows PE via MinGW (CLI-only); `llvm-tblgen`, OpenSSL, staging.
6. **[user_manual.md](user_manual.md)** — Qt GUI layout, settings, shortcuts, optional neural refine, macOS `RetDec.app`.
7. **[CLAIMS.md](CLAIMS.md)** — claims register (status + verification). Do not invent F1 numbers.
8. **[DUE_DILIGENCE.md](DUE_DILIGENCE.md)** — responses to Plan.md Part 2.
9. **[../Plan.md](../Plan.md)** — historical August 2026 review (not the live ship checklist; bannered at v2.0.24).
10. **[developer_guide.md](developer_guide.md)** — Repository layout, code style, new pipeline stages, tests, plugins, profiling.
11. **[architecture.md](architecture.md)** — Pipeline stages, libraries, managed-language dispatch.
12. **[pipeline_stage_map.md](pipeline_stage_map.md)** — Stage names ↔ source directories.
13. **[algorithm_reference.md](algorithm_reference.md)** — Formal notes on selected algorithms.
14. **[future_directions.md](future_directions.md)** — Research directions and open problems.

---

## Document index

### Root (product / legal / ops)

| Document | Audience | Contents |
|----------|----------|----------|
| [../README.md](../README.md) | Everyone | Product positioning, measured results, features, build overview |
| [../QUICKSTART.md](../QUICKSTART.md) | New users | Ten-minute decompile from v2.0.24 Release assets |
| [../ROADMAP.md](../ROADMAP.md) | Buyers / planners | Public status (LLVM Track 2; packages; leftovers) |
| [../CHANGELOG.md](../CHANGELOG.md) | Everyone | Version history; [2.0.24] is the current tag |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | Contributors | Build, tests, CLA, PR style |
| [../SECURITY.md](../SECURITY.md) | Operators / researchers | Vulnerability reporting; untrusted-input model |
| [../CODE_OF_CONDUCT.md](../CODE_OF_CONDUCT.md) | Community | Contributor Covenant 2.1 |
| [../CLA.md](../CLA.md) | Contributors | Outbound relicensing grant |
| [../LICENSING_FAQ.md](../LICENSING_FAQ.md) | Procurement | Air-gap / AGPL vs commercial (`LEG-09`) |
| [../LICENSE](../LICENSE) | Everyone | Dual AGPL-3.0+ / commercial pointer |
| [../Plan.md](../Plan.md) | Maintainers | Historical August 2026 review; live status is README / CHANGELOG [2.0.24] |

### Public technical docs (`docs/`)

| Document | Audience | Contents |
|----------|----------|----------|
| [BUILD_REFERENCE.md](BUILD_REFERENCE.md) | Everyone building | Presets, paths, superbuild, Docker, CI, `deps.cmake`, test commands |
| [INSTALL_LINUX.md](INSTALL_LINUX.md) | Linux packagers | Tarball packaging, install prefixes |
| [INSTALL_WINDOWS.md](INSTALL_WINDOWS.md) | Windows users | NSIS/portable install, PATH, smoke after install |
| [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md) | Windows developers | MSVC + Qt6 (CUDA optional), scripts, troubleshooting |
| [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md) | Linux/WSL packagers | MinGW cross, tblgen, OpenSSL, `dist/windows` |
| [user_manual.md](user_manual.md) | GUI users | Layout, panels, settings, export, shortcuts, `RetDec.app` |
| [architecture.md](architecture.md) | Contributors | Full pipeline, components, dependencies |
| [ARCHITECTURE_TARGETS.md](ARCHITECTURE_TARGETS.md) | Contributors | Native CPU maturity; SPARC/SystemZ/XCore unimplemented |
| [pipeline_stage_map.md](pipeline_stage_map.md) | Contributors | Stage names ↔ directories |
| [developer_guide.md](developer_guide.md) | Contributors | Style, tests, debugging, new stages, plugins |
| [algorithm_reference.md](algorithm_reference.md) | Researchers | Math-heavy algorithm notes |
| [SEMANTIC_OUTPUT.md](SEMANTIC_OUTPUT.md) | Output authors | C vs C++ semantics, STL recovery hints |
| [CLAIMS.md](CLAIMS.md) | Everyone quoting numbers | Claims register; withdrawn F1 / CUDA-pipe rows |
| [BENCHMARKS.md](BENCHMARKS.md) | Releases | Harness and methodology |
| [BENCHMARKS_TABLE.md](BENCHMARKS_TABLE.md) | Releases | Fork vs stock RetDec 5.0 + F1 gates |
| [NEURAL_REFINEMENT.md](NEURAL_REFINEMENT.md) | Operators | llama.cpp opt-in, gates, allowlist |
| [CUDA_CAPABILITIES.md](CUDA_CAPABILITIES.md) | Parked-research readers | CUDA vs CUDA-accel flags; unintegrated |
| [PERFORMANCE.md](PERFORMANCE.md) | Maintainers | Profiling and perf-nightly |
| [STANDALONE_CHECK.md](STANDALONE_CHECK.md) | Contributors | LLVM-free layer with just a compiler |
| [FUZZING.md](FUZZING.md) | Contributors | Parser fuzz harnesses and crash corpus |
| [VERIFICATION.md](VERIFICATION.md) | Contributors | ESBMC proofs of arithmetic kernels |
| [FORMAL_VERIFICATION_BRIDGE.md](FORMAL_VERIFICATION_BRIDGE.md) | Researchers | Bridge from decompilation to verification |
| [THREAT_MODEL.md](THREAT_MODEL.md) | Security readers | Untrusted-input model; no in-tree sandbox |
| [PROVENANCE.md](PROVENANCE.md) | Diligence | Upstream MIT + Imortek dual-licence record |
| [PROVENANCE-files.md](PROVENANCE-files.md) | Diligence | Generated file-level header classes |
| [LGPL_QT.md](LGPL_QT.md) | Packagers / counsel | Qt 6 dynamic-link and relink evidence (`LEG-12`) |
| [COMMERCIAL_WHITEPAPER.md](COMMERCIAL_WHITEPAPER.md) | Buyers | Commercial overview (must not outrun CLAIMS.md) |
| [SYMBOL_SERVER.md](SYMBOL_SERVER.md) | Windows analysts | PDB / symbol-server setup for richer names |
| [DUE_DILIGENCE.md](DUE_DILIGENCE.md) | Diligence readers | Plan.md Part 2 responses |
| [future_directions.md](future_directions.md) | Planners | Research topics (not sprint work) |
| [RESEARCH_FRONTIERS.md](RESEARCH_FRONTIERS.md) | Researchers | Long-horizon topics (not sprint work) |

### Internal / packaging / results

| Document | Audience | Contents |
|----------|----------|----------|
| [internal/README.md](internal/README.md) | Maintainers | Index of internal / historical notes |
| [internal/GUI_POLISH.md](internal/GUI_POLISH.md) | GUI contributors | Polish checklist |
| [internal/GUI_ROADMAP.md](internal/GUI_ROADMAP.md) | Product / GUI | Phased GUI plan, CI verification commands |
| [internal/ENGINEERING_ROADMAP.md](internal/ENGINEERING_ROADMAP.md) | Maintainers | Shippable engineering tiers, backlog |
| [internal/MAINTAINER_SCOPE.md](internal/MAINTAINER_SCOPE.md) | Maintainers | What is in scope vs skipped |
| [../releases/README.md](../releases/README.md) | Release managers | GitHub Releases layout, install scripts in git |
| [../results/README.md](../results/README.md) | Releases / CI | Live baselines and stock/F1 JSON |
| [../data/README.md](../data/README.md) | Maintainers | Archived JSON, logs, local dumps |
| [../scripts/README.md](../scripts/README.md) | Everyone building | Every important `scripts/*.sh` and `*.ps1` helper |

**API (Doxygen):** Configure the `doc` / `docs` CMake target if enabled (`RETDEC_DOC`); main page text is maintained under [doxygen/doxygen.h](doxygen/doxygen.h).

### Contributors — suggested reading order

1. [BUILD_REFERENCE.md](BUILD_REFERENCE.md) — configure, presets, `ctest`, CI workflows.
2. [developer_guide.md](developer_guide.md) — layout, style, tests, plugins.
3. [internal/ENGINEERING_ROADMAP.md](internal/ENGINEERING_ROADMAP.md) — what to pick up next; [internal/GUI_ROADMAP.md](internal/GUI_ROADMAP.md) / [internal/GUI_POLISH.md](internal/GUI_POLISH.md) for GUI work.
4. [architecture.md](architecture.md) + [pipeline_stage_map.md](pipeline_stage_map.md) — pipeline before touching stages.
5. [user_manual.md](user_manual.md) — expected GUI behaviour when changing `retdec-gui`.

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

Workflows live under [.github/workflows/](../.github/workflows/). The table lists every workflow file present in that directory. Full LLVM builds are not run on every push except as noted.

`release-installers.yml` builds **Windows + Linux + macOS**. The `windows-installer` job resolves the version locally (tag / dispatch input / `CMakeLists.txt`) and does **not** `needs: release`, so Ubuntu queue time cannot block the Windows zip. `linux-installer` and `macos-installer` still `needs: release`. Linux AppImage is **off** unless dispatch `appimage: true` or repository variable `APPIMAGE`.

| Workflow | Trigger | Purpose |
|----------|---------|---------|
| [ci-smoke.yml](../.github/workflows/ci-smoke.yml) | Push / PR / dispatch | Python smoke, ship checklist, cheap gates (no decompiler LLVM build) |
| [ctest-linux.yml](../.github/workflows/ctest-linux.yml) | Push / PR to `main` + dispatch | Linux build + headless GUI + `ctest` |
| [ctest-macos.yml](../.github/workflows/ctest-macos.yml) | Push / PR to `main` + dispatch | macOS arm64 `full-linux-release` + unfiltered `ctest` |
| [ctest-windows.yml](../.github/workflows/ctest-windows.yml) | Nightly + dispatch | Windows build + headless GUI + `ctest` |
| [release-installers.yml](../.github/workflows/release-installers.yml) | Tags `v*` + dispatch | GitHub Release: Windows NSIS/zip, Linux tarball, macOS tarball + `RetDec.app`; AppImage opt-in; Sigstore |
| [macos-package.yml](../.github/workflows/macos-package.yml) | Nightly + dispatch | Rehearsal of the **release** macOS configure (not `ctest-macos`'s tree); no publish |
| [fuzz-pr.yml](../.github/workflows/fuzz-pr.yml) | Push / PR / dispatch / weekly | Parser fuzz: `standalone_fuzz` on PR; LLVM `RETDEC_FUZZ` on schedule |
| [codeql.yml](../.github/workflows/codeql.yml) | Push / PR to `main`, weekly, dispatch | CodeQL for Python and Actions (not a C++ LLVM compile) |
| [doc-integrity.yml](../.github/workflows/doc-integrity.yml) | Push / PR / dispatch | Docs consistency checks; no LLVM |
| [standalone-check.yml](../.github/workflows/standalone-check.yml) | Push / PR / dispatch | LLVM-free compile + unit suites |
| [verify-esbmc.yml](../.github/workflows/verify-esbmc.yml) | Push / PR / dispatch / schedule | ESBMC proofs of arithmetic kernels |
| [sanitizers.yml](../.github/workflows/sanitizers.yml) | PR (src paths) / weekly / dispatch | ASan / UBSan |
| [coverage.yml](../.github/workflows/coverage.yml) | Weekly + dispatch | gcov/lcov via `core-coverage` (builds pinned LLVM) |
| [docker-from-release.yml](../.github/workflows/docker-from-release.yml) | Dispatch / path push / after `release-installers` | Pack published Linux tarball into GHCR (`Dockerfile.runtime`) |
| [docker-publish.yml](../.github/workflows/docker-publish.yml) | Dispatch only | 360-minute in-tree `Dockerfile` LLVM rebuild |
| [appimage-from-release.yml](../.github/workflows/appimage-from-release.yml) | Dispatch / path push / after `release-installers` | Wrap published Linux tarball as AppImage without rebuilding LLVM |
| [sign-release-sbom.yml](../.github/workflows/sign-release-sbom.yml) | Dispatch / path push / after `release-installers` | Keyless-sign CycloneDX / tarball when `.sigstore.json` is missing |
| [algorithm-recovery-nightly.yml](../.github/workflows/algorithm-recovery-nightly.yml) | Weekly + dispatch | Name-blind F1 corpus (needs a built decompiler) |
| [perf-nightly.yml](../.github/workflows/perf-nightly.yml) | Weekly + dispatch | Performance trend JSON |
| [qt-lgpl-evidence.yml](../.github/workflows/qt-lgpl-evidence.yml) | Weekly + dispatch | dumpbin `/dependents` on the published Windows portable zip |
| [cla.yml](../.github/workflows/cla.yml) | `pull_request_target` + issue comment | CLA Assistant (`cla-signatures` branch) |
| [upload-sample-binary.yml](../.github/workflows/upload-sample-binary.yml) | Dispatch / path push / after `release-installers` | Attach `fib_smoke` / `fib_smoke.exe` to the Release |

Trigger long builds from **Actions → Run workflow**, or run `ctest` locally. Details: [BUILD_REFERENCE.md](BUILD_REFERENCE.md#continuous-integration).

---

## Docker

Stock RetDec 5.0 compare **pulls** `remnux/retdec` (Windows `docker.exe`).
We do not build custom images for that table. Official Hub image
`retdec/retdec:v5.0` does not exist.

```powershell
$env:PATH = "C:\Program Files\Docker\Docker\resources\bin;" + $env:PATH
py -3 scripts\run_stock_retdec_docker.py --profile full --skip-pull
```

Fork images: `docker-from-release.yml` packs the Linux tarball into
`ghcr.io/odin-loki/retdec` (anonymous pull may still be 401). Docker Hub
`imortek/retdec` is unpublished. The in-tree [Dockerfile](../Dockerfile) is
dispatch-only (`docker-publish.yml`). See [BUILD_REFERENCE.md](BUILD_REFERENCE.md#docker)
and [internal/MAINTAINER_SCOPE.md](internal/MAINTAINER_SCOPE.md).

---

## Developer diagnostics (environment variables)

Set to any non-empty value except `0` unless noted. These are **verbose logging** switches; they may print large amounts of data.

| Variable | Area |
|----------|------|
| **`RETDEC_HEURISTIC_DIAG`** | Jump-table recovery — guessed defaults, table size mismatch |
| **`RETDEC_DECODER_TLS_DIAG`** | Decoder — TLS callback list (RVA/VA) in `initJumpTargetsTls()` |
| **`RETDEC_PARAM_RETURN_TRACE`** | `param_return` — stages + module summary |
| **`RETDEC_EMULATION_UNPACK_DIAG`** | Stage 3 emulation unpack — input path/size, entry_rva, failure reasons |
| **`RETDEC_BIN2LLVMIR_DIAG`** | `retdec` — `pass_ms <pass>=` for every LLVM pass; `pipeline_wall_ms` |
| **`RETDEC_PROFILE_JSON`** | `retdec` — write `Profiler` JSON (`auto`/`1` → `<output>.profile.json`) |
| **`RETDEC_TYPE_INFERENCE`** | `retdec` — run unused `TypeInferencePass` loop (off by default) |
| **`RETDEC_OCL_HOST`** | `retdec` — set `0` to skip OpenCL host recovery |
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
# RETDEC_ENABLE_CUDA_ACCEL defaults OFF; NVCC is not required.
bash scripts/wsl_configure_nosudo.sh   # cmake --preset full-linux-debug → build/linux/
cmake --build build/linux -j"$(nproc)"
```

Equivalent using presets only (CMake **3.26+**):

```bash
cmake --preset full-linux-release
cmake --build --preset full-linux-release
```

Run the full test suite:

```bash
ctest --test-dir build/linux --output-on-failure
```

**Test binaries** live under `build/linux/tests/<component>/` (names like `retdec-*-tests`). CMake targets are typically `tests-*`. The unpacker test target requires the unpacker tool to be enabled in the configured component set.

On macOS the same `full-linux-*` presets write `build/linux/`. A shippable
`RetDec.app` is produced by `scripts/build-macos-installer.sh`, not by
`cmake --install` alone.

---

## Native Windows build (MSVC + Qt6 GUI)

Full GUI builds must run **on Windows** with MSVC. CUDA Toolkit is optional
and unused unless you pass `-DRETDEC_ENABLE_CUDA_ACCEL=ON` (still unintegrated).
See [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md).

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
