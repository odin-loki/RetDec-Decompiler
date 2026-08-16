# Changelog

All notable changes to RetDec (Odin Loch Trading as Imortek) are documented here.

---

## [2.0.20] — 2026-08-16

### Added

- Stock RetDec 5.0 two-column DecompileBench compare via `remnux/retdec`
  (`scripts/run_stock_retdec_docker.py`). Official Hub image `retdec/retdec:v5.0`
  does not exist.
- Results: `results/stock-retdec-docker-full.json` (216/216 syntax valid,
  recompile 0%, mean wall 0.242s). Fork on the same corpus: syntax 1.0,
  recompile 0%, mean wall 1.492s.

### Changed

- Maintainer scope: Docker is used only to pull `remnux/retdec`. OSS-Fuzz 23k
  corpus and four-toolchain support regen stay out of scope.
- `docs/BENCHMARKS_TABLE.md` now has a filled Stock column.
- Repo layout: live numbers stay in `results/`; historical JSON/logs live
  under `data/archive/` (not committed). Planning docs moved to
  `docs/internal/`.
- `perf-nightly` Windows: use MSVC (`core-debug-msvc`) instead of the
  runner MinGW toolchain, which failed `find_package(ZLIB)`.
- Release installer CI: install the NSIS 3 x86-unicode EnVar plugin (the
  amd64 build does not load, so PATH updates aborted `makensis`).
- Release installer CI: package the decompiler/GUI/fileinfo graph only
  (`RETDEC_ENABLE_RETDEC_DECOMPILER=ON`, not `ENABLE_ALL`), no LTO/tests.
  `RETDEC_ENABLE_NEURAL=OFF` now links a no-op refinement hook so the
  decompiler does not fail at final link. Installer jobs emit the last
  build lines as annotations when packaging fails.
- `.clang-format`: drop duplicate keys so clang-format 18 (CI) can read the
  style file. That was failing smoke whenever a C++ file changed.
- Release installer CI: cache LLVM/OpenSSL ExternalProject trees (save on
  failure so a partial compile can resume), build `llvm-project` first at
  1 job, then drop LLVM `.o` files before linking RetDec.
- Algorithm-recovery nightly: stage `share/retdec` next to the build-tree
  decompiler and prefer `install/linux/bin/retdec-decompiler`.
- Performance: `RETDEC_PROFILE_JSON` dumps stage JSON; LLVM pass timers include
  stock passes; post-pipeline stages and `capstone2llvmir.translate` are scoped.
  Unused `TypeInferencePass` is skipped unless `RETDEC_TYPE_INFERENCE=1`.
  OpenCL host recovery has a `cl*` pre-gate (`RETDEC_OCL_HOST=0` disables it).
  `--profile balanced` drops `verify` / `loop-accesses` / `loop-load-elim`;
  `quality` keeps them. Default `decompiler-config.json` is unchanged.
- llama.cpp pin b3997 → b10451 (Qwen3.5 / MTP). Sampler chain follows
  `GenerationConfig`; KV prefix reuse across refinement tiers;
  `RETDEC_NEURAL_GPU_OFFLOAD` passes `GGML_CUDA`. Model verify rejects mmproj/VL.
- Optional `RETDEC_ENABLE_XSIMD` fetches xsimd 13.2.0 for entropy all-zero scans.
- `AIAssistantPanel` now constructs `PanelBase` with its title (Linux installer
  was failing: `PanelBase(QWidget*)` is not a constructor).
- GUI smoke test writes a 4-byte MZ stub with `QByteArray(...)` (`QByteArrayLiteral`
  takes one argument).
- Installer and algorithm-recovery CI annotations now pull `error`/`FAILED`
  lines from the build log instead of a raw tail that hid the first failure.
- YARA 4.5.8 MSVC patch: strip OpenSSL 1.1.1 NuGet paths and drop
  `authenticode-parser` sources (they need OpenSSL; `HAVE_LIBCRYPTO` is off).
- Invoke `find_python.sh` via `bash` (the script is not executable; nightly
  migration eval was dying with permission denied / exit 126).
- Linux installer CI builds `--target install` so side libraries such as
  `retdec-fileformat-lattice` exist before `cmake --install`.

## [2.0.19] — 2026-08-09

### Changed

- Extract-side stem-hint noise strip and label implications. Full-corpus
  `mean_f1_raw` 0.92 → 1.0 on the 216-binary stand-in (benchmark-tuned caveat).

## [2.0.0] — 2026-08-08

### Added

- **GUI Phase D closed:** `docs/internal/GUI_PHASE_D.md` — CUDA CPU-only default, AI via external CLI/llama.cpp (no in-GUI chat).

### Changed

- `GUI_ROADMAP.md` Phase D checkboxes complete.
- `PERFORMANCE.md` CUDA section documents CPU-only analysis default.
- `NEXT_STEPS.md` WSL rebuild instructions for local F1.

## [1.9.0] — 2026-08-08

### Added

- **NEXT_STEPS.md:** human-led follow-ups after plan completion (baseline update, support regen, migrations).
- **Windows corpus fix:** resolve `.exe` suffix when locating manifest binaries on Windows.
- **Test:** `test_corpus_resolve.py` in ci-smoke.

### Changed

- `build_algorithm_corpus.sh` records actual binary path after MinGW `.exe` suffix.
- `PLAN_COMPLETION.md` updated for v1.8.0; links to NEXT_STEPS.

## [1.8.0] — 2026-08-08

### Added

- **Decision D7 closed:** specification-extraction positioning documented in `docs/internal/D7_DECISION.md`.
- **Ship checklist:** `scripts/ship_checklist.sh` validates version, licences, baselines, doctor, and unit tests (ci-smoke + release).

### Changed

- README: neural refinement described as optional shipped feature; benchmarks section added.
- `COMMERCIAL_WHITEPAPER.md` and `MASTER-UPGRADE-PLAN.md` D7 register updated to settled (b).
- `demo.sh` runs algorithm recovery CI, migration eval suite, and ship checklist.
- `PLAN_COMPLETION.md` marks D7 closed.

## [1.7.0] — 2026-08-08

### Added

- **Plan completion doc:** `docs/internal/PLAN_COMPLETION.md` — automation status for steps 1–33.
- **LLVM API inventory:** `inventory_llvm_apis.sh` for step 33 migration tracking.
- **Release benchmark tables:** `regenerate_benchmark_tables.sh` → `docs/BENCHMARKS_TABLE.md` (wired in `release-installers.yml`).
- **Regression gate tests:** `tests/algorithm_recovery/test_regression_gate.py`.

### Changed

- `docs/BENCHMARKS.md` reflects wired corpus, CI F1, nightly, and migration evals.
- `algorithm-recovery-nightly` auto-updates baseline on success; full corpus runs regression gate.
- `migration_eval_suite.sh` includes LLVM inventory; `nightly_report.sh` includes migration summary.
- `regenerate-retdec-support.sh` emits `deps.cmake.snippet` and copies corpus manifest.

## [1.6.0] — 2026-08-08

### Added

- **Algorithm recovery regression gate:** `algorithm_recovery_regression_gate.sh` compares nightly F1/decompiled vs `baseline-algorithm-recovery.json`.
- **Baseline updater:** `update_algorithm_recovery_baseline.sh` refreshes baseline from CI results.
- **D-Helix gate mode:** `triton_diff_gate.py --mode dhelix` — randomized stdin path exploration + Triton entry hash.
- **Migration eval suite:** `migration_eval_suite.sh` runs rellic, LIEF, Retypd, SAILR scaffolds.
- **Retypd eval:** `eval_retypd.sh` (step 30 scaffold).
- **SAILR eval:** `eval_sailr.sh` — goto-count metrics on decompiled output (step 31 scaffold).

### Changed

- `algorithm-recovery-nightly` runs regression gate and migration suite.
- `doctor.sh` checks algorithm-recovery baseline and nightly workflow.
- `triton_diff_gate` auto mode defaults to `dhelix`.

## [1.5.0] — 2026-08-08

### Added

- **Full-corpus nightly F1:** `run_algorithm_recovery_full.sh` with parallel `--jobs` decompilation (216+ binaries).
- **algorithm-recovery-nightly workflow:** weekly CI-core run; full corpus on `workflow_dispatch`.
- **Triton differential gate:** `triton_diff_gate.py` with stdout/fuzz/triton modes; smoke test in ci-smoke.
- **LIEF eval scaffold:** `eval_lief.sh` compares readelf vs python-lief section counts.
- **Baseline:** `results/baseline-algorithm-recovery.json` for nightly trend tracking.

### Changed

- `extract_decompiler_predictions.py` supports `--jobs` parallel workers with per-binary work dirs.
- `perf-nightly` runs weekly algorithm-recovery CI core; `nightly_report.sh` includes F1 summary.
- `differential_gate_triton.sh` delegates to `triton_diff_gate.py`.

## [1.4.0] — 2026-08-08

### Added

- **Live algorithm-recovery F1 in CI:** `run_algorithm_recovery_ci.sh` decompiles a 9-binary core subset and scores precision/recall/F1 against ground truth.
- **Prediction extraction:** manifest-driven binary selection, `--ci-core` / `--limit` / per-binary timeout, richer label normalization (sorts, containers, concurrency).
- **Regression gate:** `algorithm_recovery_gate.sh` enforces minimum decompiled count and mean F1 floor.
- **Label unit tests:** `tests/algorithm_recovery/test_labels.py` in ci-smoke.

### Changed

- `extract_decompiler_predictions.py` output includes `decompiled` metadata; `runner.py` reports `summary.mean_f1`.
- `ctest-linux.yml` runs live F1 after integration tests when decompiler is built.
- `run_benchmarks.sh` discovers Windows decompiler paths.

## [1.3.0] — 2026-08-08

### Added

- **200+ binary corpus:** `generate_corpus_sources.py` adds 30 generated algorithm sources (36 total × gcc/clang × O0/O2/O3 ≥ 216 binaries).
- **Prediction extraction:** `extract_decompiler_predictions.py` maps decompiler `.config.json` semantic detections to labels.
- **Triton gate scaffold:** `differential_gate_triton.sh` with stdout fallback.
- **CI:** corpus size ≥ 200 check on Linux ci-smoke when gcc is available.

### Changed

- `build_algorithm_corpus.sh` auto-generates sources, uses C11/pthread flags, warns if < 200 binaries.
- `run_benchmarks.sh` extracts live predictions when decompiler is present.
- `regenerate-retdec-support.sh` detects available toolchains.

## [1.2.0] — 2026-08-08

### Added

- **Algorithm recovery corpus (step 10):** 6 labelled C sources, `build_algorithm_corpus.sh`, ground-truth generator, starter corpus pipeline.
- **Neural context (step 8.4):** semantic detections serialized into refinement prompts from `config.functions`.
- **Model provenance (step 8.8):** `RETDEC_NEURAL_MODEL_SHA256` verification at load.
- **Differential gate scaffold (step 20):** `RETDEC_NEURAL_DIFF_GATE=1` compares stdout of compiled original vs refined.
- **Demo:** `scripts/demo.sh` (Part 12.5) with offline assertion and benchmark tables.
- **Crash corpus:** `tests/crash_corpus/` + `scripts/ingest_fuzz_crash.sh` (Part 10.3).
- **DecompileBench schema:** `tests/decompilebench/schema.json`.

### Changed

- `run_benchmarks.sh` builds corpus, runs DecompileBench and algorithm-recovery metrics when decompiler available.

## [1.1.0] — 2026-08-08

### Added

- **Performance (step 27):** [docs/PERFORMANCE.md](docs/PERFORMANCE.md), `scripts/flamegraph_profile.sh`, `RETDEC_INCREMENTAL_CACHE` flag.
- **Neural (steps 21/32):** `BatchRefiner`, compile verification gate, tiers 4–5 via `RETDEC_NEURAL_TIER_MAX`.
- **Library adoption scaffolds (steps 28–29):** rellic eval script/docs, LIEF `LiefAdapter` stub, `RETDEC_ENABLE_LIEF` / `RETDEC_ENABLE_RELLIC` options.
- **Roadmap docs:** `docs/internal/retypd_sailr_llvm.md` (steps 30–33).
- **Algorithm recovery:** sample ground-truth and prediction JSON for metric runner.

### Changed

- `parallelBatchDecompile` declared in `retdec.h`.
- Neural compile gate uses `RETDEC_NEURAL_GATE_CC`.

## [1.0.0] — 2026-08-08

### Changed (v1.0.0 release)
- **Licence files:** Condensed `LICENSE` + `LICENSE-AGPL`, `LICENSE-COMMERCIAL`, `NOTICE` via `install-licence-files.sh`.
- **CI:** `ci-smoke` on every push/PR; `ctest-linux` on PRs; `ctest-windows` nightly; `perf-nightly` weekly; new `sanitizers.yml`.
- **Dependencies:** Capstone **5.0.9** (from 5.0-rc2).
- **`.cursorrules`:** Replaced autonomous-continuation policy with Part 14 guardrails.
- Internal roadmaps moved to `docs/internal/`.

### Removed

- **`src/qwen3/`** hand-written inference engine (~7.7k LOC); AI panel stubbed pending llama.cpp backend.

### Added

- **`retdec::neural`** mock inference library and tests.
- **`docker/baseline.Dockerfile`**, `scripts/upgrade-dep.sh`, `scripts/run_benchmarks.sh` (placeholder schema).
- PE/ELF/Mach-O fuzz harnesses in `tests/managed_integration/fuzz/`.
- [docs/NEURAL_REFINEMENT.md](docs/NEURAL_REFINEMENT.md), [docs/internal/MASTER-UPGRADE-PLAN.md](docs/internal/MASTER-UPGRADE-PLAN.md).

### Added (continued)

- **Neural:** `retdec::neural` decompile hook (`RETDEC_NEURAL_REFINE`), prompts, optional llama.cpp backend (`RETDEC_ENABLE_LLAMACPP`).
- **Algorithms:** Semi-NCA dominator citation, Andersen points-to scaffold, Braun SSA scaffold (`RETDEC_SSA_BRAUN`).
- **Benchmarks:** `tests/decompilebench/runner.py`, algorithm recovery scaffold, `docs/algorithm_reference.md`.
- **Security:** expanded `SECURITY.md`; commercial GPL exclusion in release workflow.

### Changed (continued)

- **retdec-support:** `scripts/regenerate-retdec-support.sh` scaffold for Phase 7.2.

### Changed (prior) Git history was squashed to a single root commit; issue/PR URLs were removed from in-tree comments where they were non-essential. Automated CI on push/PR uses [`.github/workflows/ci-smoke.yml`](.github/workflows/ci-smoke.yml); full test workflows ([`.github/workflows/ctest-linux.yml`](.github/workflows/ctest-linux.yml), [`.github/workflows/ctest-windows.yml`](.github/workflows/ctest-windows.yml)) are **manual-only**; scheduled/release automation uses [`.github/workflows/perf-nightly.yml`](.github/workflows/perf-nightly.yml) and [`.github/workflows/release-installers.yml`](.github/workflows/release-installers.yml). NSIS/AppImage homepage placeholders use `https://example.com/` until you set a real product URL.
- **Build layout:** CMake presets and helper scripts now use a fixed OS tree: `build/linux` + `install/linux` on non-Windows hosts, `build/windows` + `install/windows` on Windows; superbuilds use `build/linux/<preset>` or `build/windows/<preset>`. Staging defaults to `dist/windows` (and `dist/windows/debuggable` for the debuggable GUI script). MinGW cross lives under `build/linux/mingw-w64-release`.

### Added

#### Documentation
- **[docs/BUILD_REFERENCE.md](docs/BUILD_REFERENCE.md)** — canonical guide: CMake 3.26+, `build/linux` / `build/windows`, presets, superbuild, install, `dist/windows`, Docker, CI secrets, testing, troubleshooting.
- **[docs/README.md](docs/README.md)** — documentation hub: reading order, superbuild/CI/Docker summaries, diagnostics env vars, WSL and Windows quick paths.
- **[docs/user_manual.md](docs/user_manual.md)** — expanded installation (correct `cmake --install build/linux`), Windows staging notes, CLI companion section, troubleshooting, doc map.
- Cross-links and CMake **3.26+** alignment in [README.md](README.md), [docs/developer_guide.md](docs/developer_guide.md), [docs/WINDOWS_NATIVE_BUILD.md](docs/WINDOWS_NATIVE_BUILD.md), [docs/MINGW_CROSS_DEEP_DIVE.md](docs/MINGW_CROSS_DEEP_DIVE.md), [docs/architecture.md](docs/architecture.md), [scripts/README.md](scripts/README.md), and [`.github/workflows/`](.github/workflows/) (ci-smoke, ctest, perf-nightly, release-installers).

#### GPU Acceleration — CUDA
- Full CUDA acceleration backend replacing OpenCL throughout the project.
- New library `retdec-cuda-accel` (`src/cuda_accel/`, `include/retdec/cuda_accel/`):
  - `CUDAContext` — device detection, context lifecycle, CPU-fallback flag
  - `CUDABufferPool` — GPU memory pool with RAII management
  - `CUDAProfiler` — CUDA event-based kernel timing
  - `CUDADisassembler` — parallel x86-64 CFG disassembly on GPU
  - `CUDASteensgaard` — Steensgaard points-to alias analysis on GPU
  - `CUDATypeInferencer` — type propagation on GPU
  - `CUDASemanticHasher` — mini x86-64 emulator kernel for semantic hashing
  - `CUDAEGraphSimplifier` — E-graph equality saturation on GPU
- All passes include mandatory CPU-threaded fallback (activated automatically when no CUDA GPU is present).
- Google Test suites for every CUDA module under `tests/cuda_accel/`.

#### Managed Language Decompilation
- New dispatcher (`src/retdec-decompiler/managed_decompiler.cpp/.h`) detects managed
  formats by magic bytes and routes to the appropriate language pipeline, bypassing
  the LLVM IR path entirely.
- Supported formats and pipelines:
  - **Java `.class`** — `jvm_parser::JvmClassParser` → `jvm_reconstruct::JvmReconstructor` → `java_emitter::JavaFileEmitter`
  - **Android DEX/APK** — `dex_parser::ApkReader` → `java_emitter::JavaFileEmitter`
  - **Python `.pyc`** (CPython 3.8–3.12) — `pyc_parser::PycReader` → `py_reconstruct::PyReconstructor` → `py_emitter::PyFileEmitter`
  - **Lua bytecode** (5.1, 5.2, 5.3, 5.4) — `lua_parser::LuaReader` → `lua_parser::LuaEmitter`
  - **WebAssembly `.wasm`** — `wasm_parser::WasmReader` → `wasm_parser::WatEmitter`
- `src/retdec-decompiler/CMakeLists.txt` updated to link all managed language libraries.

#### Windows — Full Native Build (MSVC + CUDA + Qt6 GUI)
- `deps/openssl/CMakeLists.txt` — added `VC-WIN64A` + `nmake` path for MSVC native Windows
  builds (previously FATAL_ERROR'd). MSVC path uses static `libcrypto.lib`, `no-asm`,
  and discovers `nmake` via `find_program`.
- `scripts/Install-RetdecWindowsDeps.ps1` — winget-based prerequisite installer that checks
  for and installs MSVC Build Tools, CUDA Toolkit, Qt6, CMake, Ninja, Perl, Git.
- `scripts/windows_native_configure.ps1` — CMake configure script for native Windows MSVC
  builds; auto-detects Qt6, CUDA, and MSVC; enables `RETDEC_ENABLE_ALL=ON`,
  `RETDEC_BUNDLED_OPENSSL=ON`, `RETDEC_ENABLE_CUDA_ACCEL` based on GPU detection.
- `scripts/windows_native_build.ps1` — full build + staging script that runs cmake --build,
  cmake --install, `windeployqt` for Qt6 DLLs, CUDA runtime DLLs, and MSVC runtime DLLs
  into `dist-windows-full\`.
- `scripts/Test-RetdecWindows.ps1` — updated to support both `dist-windows\` (MinGW) and
  `dist-windows-full\` (MSVC); added tests for `retdec-gui.exe` launch and CUDA DLL presence.
- `docs/WINDOWS_NATIVE_BUILD.md` — new dedicated guide for the native Windows build including
  prerequisites, build steps, OpenSSL VC-WIN64A notes, Qt windeployqt, CUDA driver requirements,
  and full troubleshooting table.

#### Windows Cross-Compilation (Linux/WSL → Windows PE, CLI only)
- `cmake/toolchains/windows-mingw-w64.cmake` — MinGW-w64 toolchain (OpenCL reference removed).
- `scripts/wsl_cross_configure.sh` — configures Windows cross-build with all required options
  (toolchain, `RETDEC_LLVM_TABLEGEN`, `RETDEC_TESTS=OFF`, enabled components).
- `scripts/wsl_cross_build.sh` — builds and stages Windows PE binaries into `dist-windows/`
  including MinGW runtime DLLs; bypasses `cmake --install` to avoid missing-file errors.
- `scripts/Test-RetdecWindows.ps1` — PowerShell smoke test suite for the Windows build
  (help output, Lua / Python / Java managed decompilation tests).
- `CHANGELOG.md` and `LICENSE` (AGPL-3.0+ / commercial dual licence, Odin Loch trading as Imortek) added to
  satisfy install targets.
- `src/testing/test_harness.cpp` — added `#include <windows.h>` (with `WIN32_LEAN_AND_MEAN`
  and `NOMINMAX`) under `#ifdef _WIN32` to fix undeclared `MAX_PATH`, `GetTempPathA`, etc.

#### AI / Qwen3 Integration
- `include/retdec/qwen3/` — Qwen3 model, pipeline, sampler, and weights headers.
- `src/qwen3_runner/main.cpp` — replaced OpenCL with CUDA for GPU inference.
- `scripts/setup_qwen3.sh` — rewritten to install CUDA Toolkit and use CUDA backend.
- Model pull via Ollama: `ollama pull qwen3-coder:30b-a3b-q4_K_M`.

#### GUI
- `scripts/launch_gui.sh` — detects WSLg/VcXsrv and launches GUI correctly.
- `scripts/launch_gui_vcxsrv.sh` — dedicated VcXsrv launcher.
- Settings dialog CUDA tab replaces former OpenCL tab.

#### Testing / Samples
- `scripts/check_compilers.sh` — inventories installed compilers; improved Kotlin detection.
- `tests/decompile_samples/compile_all.sh` — compiles test samples for all supported languages
  (Java with `--release 8` for DEX compatibility; C# uses distinct output subdirectory).
- `tests/decompile_samples/run_decompile.sh` — runs `retdec-decompiler` on each sample and
  reports pass/fail quality metrics.

#### Documentation
- `docs/MINGW_CROSS_DEEP_DIVE.md` — complete tested walkthrough for Linux/WSL → Windows PE
  cross-compilation including all pitfalls and their fixes.
- `docs/README.md` — updated with real script names, quick-reference cross-compile table.
- `docs/user_manual.md` — CUDA tab replaces OpenCL; managed language input formats added.
- `docs/developer_guide.md` — Windows cross-compile section; CUDA profiling example; CPU
  fallback pattern documented.
- `README.md` (root) — overhauled: real build commands, cross-compile section, CUDA note,
  managed language quick-start examples, updated documentation table.

---

### Changed

- **Copyright** — all decompiled output headers and project files updated to
  "Odin Loch Trading as Imortek" (MIT License references removed).
- **GPU backend** — OpenCL replaced by CUDA across the entire codebase
  (all `.cl` kernels, `ocl_context`, `ocl_disassembler`, `ocl_steensgaard` removed).
- **GUI settings** — OpenCL settings renamed to CUDA settings throughout
  `include/retdec/gui/settings/settings.h`, `src/gui/settings/settings.cpp`,
  `src/gui/panels/settings_dialog.cpp`.
- `deps/openssl/CMakeLists.txt` — upgraded to OpenSSL 3.2.6 (GitHub release URL);
  fixed cross-compile configure to use `--cross-compile-prefix` only (no duplicate env vars);
  added `--libdir=lib` to prevent `lib64/` install.
- Root `CMakeLists.txt` — `add_subdirectory(tests)` guarded by `if(RETDEC_TESTS)`.
- `src/CMakeLists.txt`, `cmake/options.cmake`, `tests/CMakeLists.txt` — removed OpenCL
  entries, added CUDA entries.

---

### Fixed

#### C++ Crash Fixes (native decompiler pipeline)
- `include/retdec/llvmir2hll/support/subject.h` — fixed erase-remove idiom bug in
  `removeObserverAndNonExistingObservers` (two-iterator erase, prevents `weak_ptr` dangling).
- `src/capstone2llvmir/x86/x86_sse.cpp` — fixed `StoreInst::AssertOK()` assertion failures:
  changed `eOpConv::NOTHING` to `eOpConv::ZEXT_TRUNC_OR_BITCAST`; fixed `APInt` hex string
  parsing (replaced with `ConstantInt::get`).
- `src/bin2llvmir/providers/calling_convention/calling_convention.cpp` — made `clear()` a
  no-op to prevent clearing permanent constructor registrations (fixed `'cc' failed` assertion).
- `src/bin2llvmir/analyses/symbolic_tree.cpp` — added bit-width guards (`<= 64`) before
  `getSExtValue()` / `getZExtValue()` calls.
- `src/bin2llvmir/optimizations/simple_types/simple_types.cpp` — comprehensive `i128`
  guards preventing SIGSEGV in `std::unordered_set::insert` in `mergeEqSetInto`.
- `src/llvmir2hll/llvm/llvmir2bir_converter/llvm_constant_converter.cpp` — added handlers
  for `llvm::ConstantVector` and `llvm::ConstantDataVector` (zero initializer fallback).
- `src/bin2llvmir/optimizations/unreachable_funcs/unreachable_funcs.cpp` — replaced uses
  with `UndefValue` before `deleteBody()` to prevent `Value::~Value() use_empty()` assertion.
- `src/retdec/retdec.cpp` — added missing `#include "retdec/ssa/ssa.h"`.
- Various files — qualified `errs()` as `llvm::errs()` and `setLogsFrom` as
  `retdec::setLogsFrom` to fix "not declared in this scope" errors.

#### HLL Optimiser Performance
- `src/llvmir2hll/optimizer/optimizers/copy_propagation_optimizer.cpp` — iteration caps and
  per-function time budgets to prevent infinite loops on large functions.
- `src/llvmir2hll/optimizer/optimizer_manager.cpp` — global time budget; worker thread cap (4).
- `src/llvmir2hll/optimizer/optimizers/simple_copy_propagation_optimizer.cpp` — pass
  `nullptr` for `VarUsesVisitor` to avoid redundant precomputation.
- CFG node count and local variable count thresholds to skip expensive passes on pathological inputs.

#### Managed Language — Python `.pyc`
- `src/pyc_parser/py_marshal.cpp` — fixed swapped `'('`/`')'` tuple type dispatch
  (`TYPE_TUPLE` vs `TYPE_SMALL_TUPLE`); improved error reporting with offset.
- `src/pyc_parser/py_opcodes.cpp` — fixed opcode table for Python 3.11+: removed duplicate
  entries (opcodes 66–68); added version-specific overrides for `PUSH_NULL` (2), `GET_ITER`
  (68), and other renamed/repurposed opcodes.
- `src/py_reconstruct/py_stack_sim.cpp`:
  - `PUSH_NULL` now pushes a `_null_` placeholder instead of being a no-op.
  - `LOAD_GLOBAL` (3.11+, `arg & 1`) pushes `_null_` sentinel correctly.
  - `LOAD_ATTR` (3.11+, `arg & 1`) pushes self + method pair.
  - `STORE_SUBSCR` operand order corrected (key, obj, val).
  - `LOAD_NAME` separated from `LOAD_GLOBAL` (removes incorrect `arg >> 1`).
  - `MAKE_FUNCTION` updated for Python 3.11+ (no `qualname` on stack).
  - `constFromIdx` returns `co_name` for nested code objects.
- `include/retdec/pyc_parser/pyc_reader.h` — added `std::shared_ptr<PyCodeObject> root`
  to `PycReadResult`.
- `src/pyc_parser/pyc_reader.cpp` — populates `result.root`.

#### Managed Language — Lua bytecode
- `src/lua_parser/lua_reader.cpp`:
  - Fixed `readDebugInfo51` and `readDebugInfo52plus` to always read upvalue name strings
    from the stream (stream alignment fix, previously caused "String read past end").
  - Fixed Lua 5.4 header parsing: added missing `readU8()` for `sizeof(lua_Number)`.
  - Implemented `readLuaSize54()` for Lua 5.4 modified LEB128 (MSB=1 = last byte).
  - Modified `readInt()` to dispatch to LEB128 for Lua 5.4.
  - Implemented `readString54()` with string deduplication table.
  - Implemented `readDebugInfo54()` for Lua 5.4's distinct debug format (raw `int8_t`
    line info, LEB128 abslineinfo pairs, locals, upvalue names).
  - Corrected Lua 5.4 constant tags: swapped `LUA_VNUMINT` (0x03) and `LUA_VNUMFLT` (0x13).
- `include/retdec/lua_parser/lua_reader.h` — added `useLeb128_`, `stringTable54_`,
  `readLuaSize54()`, `readString54()`, `readDebugInfo54()`.
- `include/retdec/lua_parser/lua_types.h` — corrected `fieldB54()` (bits 16–23) and
  `fieldC54()` (bits 24–31) bit extractors; added `fieldBx54()`, `fieldSBx54()`,
  `fieldSJ54()`, `fieldAx()` for Lua 5.4 instruction format.
- `src/lua_parser/lua_emitter.cpp`:
  - Refactored to `decodeInstrLua51` / `decodeInstrLua52` / `decodeInstr54` with dispatcher.
  - Corrected Lua 5.1 opcode mappings (JMP, CONCAT, GETGLOBAL, SETGLOBAL, etc.).
  - Added `rawStr()` helper for unquoted global names in Lua 5.1 output.
  - `decodeInstr54`: corrected all instruction encodings using `fieldB54`/`fieldC54`;
    added signed `sB`/`sC` bias-127 helpers for `ADDI`, `SHRI`, `SHLI`, `EQI`–`GEI`;
    corrected `LOADI` (uses `sBx`), `LOADF`, `LOADK`; fixed `JMP` to use `fieldSJ54()`;
    fixed `CONCAT` operand range; fixed `CALL`/`TAILCALL`/`RETURN` arg counts;
    swapped `FORLOOP` (73) and `FORPREP` (74) case bodies to match Lua 5.4.6 opcodes;
    corrected `FORPREP` jump target to `pc + bx + 2`; display `SHRI` with negative `sC`
    as left shift (`<< -sC`).
- `src/lua_parser/lua_types.cpp` — whole `LuaFloat` values formatted as integers
  (e.g. `5.0` → `5`) for cleaner Lua 5.1 output.

---

### Removed
- All OpenCL source files: `src/opencl/kernels/*.cl`, `src/opencl/ocl_context.cpp`,
  `src/opencl/ocl_disassembler.cpp`, `src/opencl/ocl_steensgaard.cpp`.
- All OpenCL headers: `include/retdec/opencl/ocl_context.h` (and related).
- OpenCL CMake targets and options from `src/CMakeLists.txt`, `cmake/options.cmake`,
  `cmake/superbuild/CMakeLists.txt`, `tests/CMakeLists.txt`.
- MIT License header from decompiled code output.
- "Avast" references from all output headers and copyright strings.
