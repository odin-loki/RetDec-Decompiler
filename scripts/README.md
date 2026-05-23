# Scripts layout

Paths are relative to the **repository root**. See [docs/BUILD_REFERENCE.md](../docs/BUILD_REFERENCE.md) for the full matrix.

| Path | Purpose |
|------|---------|
| `build/linux/` | CMake binary dir on Linux / WSL / macOS (root [CMakePresets.json](../CMakePresets.json) `base`) |
| `build/windows/` | CMake binary dir on Windows (same presets) |
| `build/linux/<superbuild-preset>/` , `build/windows/...` | Superbuild: `cmake -S cmake/superbuild` + [cmake/superbuild/CMakePresets.json](../cmake/superbuild/CMakePresets.json) |
| `build/linux/mingw-w64-release/` | MinGW cross tree (`wsl_cross_configure.sh`; not a single root preset name) |
| `install/linux/` , `install/windows/` | Default install prefixes for host OS |
| `install/linux/mingw-w64-release/` | MinGW install prefix |
| `dist/windows/` | Portable PE staging (`windows_native_build.ps1`, `wsl_build.sh`, `wsl_cross_build.sh`) |
| `dist/windows/debuggable/` | Debuggable GUI bundle (`windows_prepare_debuggable_gui.ps1`) |

## Windows (MSVC)

| Script | Role |
|--------|------|
| `retdec-paths.ps1` | Dot-source: `Get-RetDecRepoRoot`, `Get-RetDecBuildDir`, VS Dev Shell |
| `build-install-run-windows.ps1` | `cmake --preset`, build, install, run runner/GUI/tests |
| `windows_native_configure.ps1` | Toolchain env + `cmake --preset full-windows-release` (CUDA + Qt6 by default; `-NoCuda` / `-AllowOptionalQt` to relax) |
| `windows_native_build.ps1` | Build, `cmake --install` → `install/windows/`, stage `dist/windows/` |
| `windows_prepare_debuggable_gui.ps1` | PDB / debuggable GUI bundle |
| `run-gui-headless-debug.ps1` | Qt offscreen + optional Qwen trace |
| `run-qwen3-trace.ps1` | `RETDEC_QWEN3_TRACE` runner wrapper |
| `Install-RetdecWindowsDeps.ps1` | winget dependency check |
| `Test-RetdecWindows.ps1` | Smoke tests against a dist folder |
| `run_gui_with_procdump.ps1` / `windows_analyze_crash_dump.ps1` | Crash capture |
| `profile_all.bat` | Baseline/verbose decompile profiling; defaults to `dist\windows`, falls back to `build-win\win-runtime` (`DECOMPILER_DIR` overrides) |
| `superbuild-build-all-windows.ps1` | Superbuild (MSVC): configure+build `superbuild-debug` and `superbuild-release` → `build\windows\<preset>\` |

## Linux / WSL

| Script | Role |
|--------|------|
| `lib/retdec-env.sh` | Source first: `RETDEC_ROOT`, `RETDEC_BUILD_DEBUG`, `RETDEC_BUILD_MINGW`, … |
| `wsl_configure.sh` | Optional `apt` deps + `wsl_configure_nosudo.sh` |
| `wsl_configure_nosudo.sh` | `cmake --preset full-linux-debug` → `build/linux/` |
| `wsl_build_and_test.sh` | Build + `ctest` for `full-linux-debug` |
| `wsl_build.sh` | Preset Linux build + MinGW cross → `build/linux/mingw-w64-release`, stage `dist/windows/` |
| `wsl_cross_configure.sh` / `wsl_cross_build.sh` | MinGW-only cross (needs native `llvm-tblgen`) |
| `check_format.sh` | Dry-run `clang-format` on tracked `include/`, `src/`, `tests/` sources |
| `build_and_test.sh` | WSL: native debug build + tests, then MinGW cross-compile and stage `dist/windows/` |
| `test_windows.bat` | Smoke tests in `dist/windows` (or set `RETDEC_WIN_RUNTIME`); run from repo: `scripts\\test_windows.bat` |
| `run_coverage.sh` | `core-coverage` preset + lcov HTML under `docs/coverage/` |
| `run_all_tests.sh` | CTest + optional PE smoke / Valgrind; auto-picks `build/linux` or `build/`; smoke PE from `dist/windows` or legacy `build-win/...` (`BUILD`, `SMOKE_BIN` override) |
| `run_asan.sh` | ASan+LSan decompiler run; binary under `build/linux` or `build/`, test PE under `dist/windows` or `build-win/win-runtime` |
| `superbuild-build-all-linux.sh` | Superbuild (GCC): `superbuild-debug` + `superbuild-release` under `build/linux/<preset>/`; optional `SUPERBUILD_MINGW=1`, `SUPERBUILD_CLANG=1` |

## CI / tooling

| Script | Role |
|--------|------|
| `fetch-large-files.ps1` / `fetch-large-files.sh` | Download support files omitted from git (required before first build) |
| `retdec_cli.py` | Unified CLI: batch decompile, diff, emit-json, export-intel, watch, yara-bridge |
| `unpack_and_decompile.ps1` / `unpack_and_decompile.sh` | Unpack (when needed) then decompile a binary |
| `validate_pipeline_json.py` | Validate pipeline JSON against `docs/pipeline_builder_schema.json` |
| `perf_bench_ci.ps1` | Time decompiler on a fib fixture; emit JSON for CI trend tracking |
| `parity_bench.ps1` | Compare CLI vs GUI subprocess wall time on a fixed binary |
| `install_smoke.ps1` | Smoke-test a `cmake --install` tree (decompiler + fileinfo) |
| `build-all.ps1` / `build-all.sh` | End-to-end configure, build, install, and package (Windows / Linux) |
| `build-windows-installer.ps1` | Stage portable zip + optional NSIS installer under `dist/` |
| `build-linux-installer.sh` | `cmake --install` + portable tarball (optional AppImage / `.deb`) |

On Linux or WSL clones, mark shell entrypoints executable once after checkout:

```bash
chmod +x scripts/*.sh scripts/lib/*.sh
```

## Other

Coverage, ASan, AppImage, model download, type_extractor, and MinGW superbuild helpers live here; prefer **CMake presets** for new workflows.

Ad hoc corpus/coverage/debug scripts used by maintainers live under [`../tools/dev/`](../tools/dev/README.md), not in CI.

Legacy one-shot `fix_*.py` helpers resolve paths from the repository root (parent of `scripts/`), not from fixed `/mnt/c/...` paths.
