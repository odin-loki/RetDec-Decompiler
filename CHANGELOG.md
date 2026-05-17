# Changelog

All notable changes to RetDec (Odin Loch Trading as Imortek) are documented here.

---

## Unreleased

### Changed

- **Repository metadata:** Git history was squashed to a single root commit; issue/PR URLs were removed from in-tree comments where they were non-essential. CI regression clones now require repository secrets `RETDEC_REGRESSION_TESTS_GIT_URL` and `RETDEC_REGRESSION_FRAMEWORK_GIT_URL` (see `.github/workflows/retdec-ci.yml`). NSIS/AppImage homepage placeholders use `https://example.com/` until you set a real product URL.
- **Build layout:** CMake presets and helper scripts now use a fixed OS tree: `build/linux` + `install/linux` on non-Windows hosts, `build/windows` + `install/windows` on Windows; superbuilds use `build/linux/<preset>` or `build/windows/<preset>`. Staging defaults to `dist/windows` (and `dist/windows/debuggable` for the debuggable GUI script). MinGW cross lives under `build/linux/mingw-w64-release`.

### Added

#### Documentation
- **[docs/BUILD_REFERENCE.md](docs/BUILD_REFERENCE.md)** — canonical guide: CMake 3.26+, `build/linux` / `build/windows`, presets, superbuild, install, `dist/windows`, Docker, CI secrets, testing, troubleshooting.
- **[docs/README.md](docs/README.md)** — documentation hub: reading order, superbuild/CI/Docker summaries, diagnostics env vars, WSL and Windows quick paths.
- **[docs/user_manual.md](docs/user_manual.md)** — expanded installation (correct `cmake --install build/linux`), Windows staging notes, CLI companion section, troubleshooting, doc map.
- Cross-links and CMake **3.26+** alignment in [README.md](README.md), [docs/developer_guide.md](docs/developer_guide.md), [docs/WINDOWS_NATIVE_BUILD.md](docs/WINDOWS_NATIVE_BUILD.md), [docs/MINGW_CROSS_DEEP_DIVE.md](docs/MINGW_CROSS_DEEP_DIVE.md), [docs/architecture.md](docs/architecture.md), [scripts/README.md](scripts/README.md), and [.github/workflows/retdec-ci.yml](.github/workflows/retdec-ci.yml).

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
- `CHANGELOG.md` and `LICENSE` (BSD 3-Clause, Odin Loch Trading as Imortek) added to
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
