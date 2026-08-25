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
| `build-install-run-windows.ps1` | `cmake --preset`, build, install, run `retdec-decompiler` / GUI / tests |
| `windows_native_configure.ps1` | Toolchain env + `cmake --preset full-windows-release` (Qt6 required; `RETDEC_ENABLE_CUDA_ACCEL` stays OFF unless you pass it ON; `-NoCuda` skips the NVCC probe) |
| `windows_native_build.ps1` | Build, `cmake --install` → `install/windows/`, stage `dist/windows/` |
| `windows_prepare_debuggable_gui.ps1` | PDB / debuggable GUI bundle |
| `run-gui-headless-debug.ps1` | Qt offscreen + `RETDEC_GUI_HEADLESS` |
| `run-qwen3-trace.ps1` | leftover wrapper for withdrawn `retdec-qwen3-runner` (does not ship). Neural path is `RETDEC_NEURAL_REFINE` |
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

| Script / workflow | Role |
|--------|------|
| `ci/benchmark_rename_guard.sh` | B6: decompile named vs SHA-256-renamed corpus copies; fail if detections appear only on the named file |
| `doctor.ps1` / `doctor.sh` | Read-only prerequisite check: CMake 3.26+, fetch-large-files marker, Qt6 hint, git-lfs, python3, perl; Windows also checks NSIS/makensis and EnVar |
| `fetch-large-files.ps1` / `fetch-large-files.sh` | Download support files omitted from git (~60 MiB; required before first build). Use `bash scripts/fetch-large-files.sh` when `.sh` is not executable |
| `.github/workflows/ci-smoke.yml` | Lightweight CI on `main` push/PR: fetch-large-files, CLI helper tests, pipeline/semantic JSON validation (no full compile) |
| `validate_pipeline_json.py` | Validate pipeline JSON against `docs/pipeline_builder_schema.json` (`python3 scripts/validate_pipeline_json.py --all-profiles`) |
| `parity_bench.ps1` | Compare CLI vs GUI subprocess wall time on a fixed binary; `-Help` for options |
| `install_smoke.ps1` | Smoke-test a `cmake --install` tree (decompiler + fileinfo on a fixture PE) |
| `perf_bench_ci.ps1` | Time decompiler on a fib fixture; emit JSON for CI trend tracking |
| `retdec_cli.py` | Unified CLI: batch decompile, diff, emit-json, export-intel, watch, yara-bridge |
| `unpack_and_decompile.ps1` / `unpack_and_decompile.sh` | Unpack (when needed) then decompile a binary |
| `build-all.ps1` / `build-all.sh` | End-to-end configure, build, install, and package (Windows / Linux) |
| `build-windows-installer.ps1` | Stage portable zip + optional NSIS installer under `dist/` |
| `build-linux-installer.sh` | `cmake --install` + portable tarball (optional AppImage / `.deb`) |
| `run_stock_retdec_docker.py` / `.sh` | Stock RetDec 5.0 compare via `remnux/retdec` (Windows `docker.exe`) |
| `_stage_stock_docker_corpus.py` | Copy real ELF files for the stock Docker mount (dereferences WSL/OneDrive links) |
| `simulate_raw_refine.py` / `reprocess_predictions_raw.py` | Offline label refine / re-score (no decompiler) |
| `analyze_full_f1.py` | Summarize algorithm-recovery F1 results |

On Linux or WSL clones, shell scripts do **not** need `chmod +x` if you invoke them with `bash scripts/<name>.sh`. To run directly (`./scripts/...`), mark entrypoints executable once after checkout:

```bash
chmod +x scripts/*.sh scripts/lib/*.sh
```

## Other

Coverage, ASan, AppImage, model download, type_extractor, and MinGW superbuild helpers live here; prefer **CMake presets** for new workflows.

Ad hoc corpus/coverage/debug scripts used by maintainers live under [`../tools/dev/`](../tools/dev/README.md), not in CI.

Live benchmark JSON: [`../results/`](../results/README.md). Archived dumps/logs: [`../data/`](../data/README.md).

Legacy one-shot `fix_*.py` helpers resolve paths from the repository root (parent of `scripts/`), not from fixed `/mnt/c/...` paths.

## CUDA keyring (do not commit `.deb` files)

`scripts/cuda-keyring_1.1-1_all.deb` was removed from the tree (E16). Fetch NVIDIA's
network-repo keyring by URL when you need CUDA apt packages; do not re-commit the binary.

```bash
# Example (Ubuntu 22.04 amd64). Pick the repo dir that matches your distro:
#   https://developer.download.nvidia.com/compute/cuda/repos/
URL="https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb"
curl -fsSL "$URL" -o /tmp/cuda-keyring_1.1-1_all.deb
# SHA-256 of the package previously committed in this repo (verify against NVIDIA if it drifts):
echo "7c0a531d0662bbd7ae233b9c55eb2a46e36735d395693c29815b45723f83a6d1  /tmp/cuda-keyring_1.1-1_all.deb" | sha256sum -c
sudo dpkg -i /tmp/cuda-keyring_1.1-1_all.deb
```

See [NVIDIA CUDA Linux install — network repo](https://docs.nvidia.com/cuda/cuda-installation-guide-linux/#network-repo-installation-for-ubuntu).
