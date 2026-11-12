# Testing

## Concepts
- Each `tests/test_*.cpp` builds into its own executable and ctest entry of the same name. It runs its `TEST(...)` cases (`tests/check.hpp`), prints `[ ok ]` or `[FAIL]` per case and a failure count, and exits 1 on any failure.
- **Exit code 77 means skipped** (missing hardware, runtime, SDK or data). Hardware tests declare `SKIP_RETURN_CODE 77`.
- `FORGE_TEST_ONLY=<substring>` runs only the cases whose name contains it (useful for locating a hang).
- **The oracle** (`verify/include/forge/verify/oracle.hpp`): every SIMD, GPU and quantised lowering is compared against the scalar reference in `ops/` within each op's declared tolerance. `test_graph`'s `oracle_matrix_all_ops` covers every registered lowering of every op; `test_cuda_hip` does the same for each GPU runtime.
- ctest runs most tests with the source tree as the working directory.

## Kinds of test

| Kind | Where | ctest names / label |
|------|-------|---------------------|
| Unit and differential | `tests/test_*.cpp` | `test_<name>` |
| Script golden tests | `tests/script/*.fs` with expected `*.out` | inside `test_script_golden`: each script runs on the tree-walker and the VM, and both outputs must match the `.out` file (`FORGE_GOLDEN_UPDATE=1` rewrites them) |
| Golden models | `verify/golden/tiny_llama.py`, `tiny_moe.py`, `tiny_qwen3.py`, `tiny_qwen3_5.py`, `quant_inplace.py` | `golden_tiny_llama`, `golden_moe_mla`, `golden_tiny_qwen3`, `golden_tiny_qwen3_5`, `golden_quant_inplace`; `golden_tiny_llama_vulkan` and `golden_moe_mla_vulkan` (label `gpu`) when Vulkan is built. `golden_tiny_llama` writes `build/<dir>/golden/tiny_f32.fpk`, which later tests use |
| Real GPU runtimes | `tests/test_cuda_hip.cpp` | `test_gpu_{cuda,hip,opencl,ze,metal}`, `test_gpu_features_*`, label `gpu`; skip without a driver and device |
| Mock GPU drivers | `tests/mock_cuda.c` (also built as the HIP mock), `mock_opencl.c`, `mock_ze.c` stand in for the vendor libraries | `test_gpu_*_mock`, `test_gpu_features_*_mock`, `test_gpu_memory_*_mock`, `golden_tiny_llama_{cuda,hip,opencl,ze}_mock`, label `gpu`. Not built when cross-compiling |
| Vulkan | `tests/test_vulkan.cpp` | `test_vulkan`, `test_vulkan_align256` (`FORGE_VK_SSBO_ALIGN=256`), label `gpu` |
| GPU compile checks | generated kernels compiled with clang | `gpu_compile_{cuda,hip,opencl,opencl_nofp64,metal_syntax}`, and `gpu_compile_metal` with `xcrun`; label `gpu` |
| Script tests | `tests/*.py` | `modelfile_signing`, `onnx_import`, `python_bindings`, `serve_http`, `sbom_spdx` |
| C ABI | `tests/capi_smoke.c` | `capi_smoke` (not built when cross-compiling) |
| TLS-only RPC cases | `test_rpc` with `FORGE_TEST_ONLY=rpc_tls` | `test_rpc_tls` (skips in a build without OpenSSL, or without the `openssl` command) |
| Log strip | `tests/log_min_level.cmake` | `log_min_level_cmake` |
| Doc examples | `docs/examples/*` | `doc_example_*` |
| Integration examples | `examples/integrations/*.cpp` | `example_*`, label `integration` |
| Docs freshness | `tools/gen_docs.py --check` | `docs_up_to_date` (fails if `docs/reference.md` is stale) |
| Fuzz | `fuzz/fuzz_*.cpp` (blueprint, fpk, gguf, grammar, json, onnx, safetensors, script, tokenizer) | `fuzz_*`, label `fuzz` |
| JNI syntax | `bindings/android/jni/forge_jni.c` against a JDK's `jni.h` | `jni_syntax` |
| Mobile | `tests/mobile_build.sh` | `mobile_android_{arm64-v8a,x86_64}`, `mobile_android_device`, `mobile_ios`, labels `mobile;slow` |
| Cross compile/link | zig cross builds | `xos_*`, labels `cross;slow` |
| Scalable vectors under qemu | `test_simd`, `test_tile` | `lane_sve_vl<N>`, `lane_rvv_vl<N>`, `tile_sme_svl<N>` |
| Emulated AMX tiles | Intel SDE | `tile_amx_sde_*`, label `emu` |

The `gpu_compile_*`, `jni_syntax` and `mobile_*` tests are only defined when CMake finds a GPU-capable clang (`FORGE_GPU_CLANG`).

Tests whose Python modules are missing are marked "Not Run (Disabled)" at configure time rather than failing. The configure log lists them (`FORGE: python lacks one of {...}`).

## Running
```sh
ctest --test-dir build/dev -j4                      # everything configured
ctest --test-dir build/dev -j4 -LE "slow|emu"       # what CI runs: skips the long cross/mobile/emulator tests
ctest --test-dir build/dev -R doc_example           # the documentation examples
ctest --test-dir build/dev -L gpu                   # GPU runtimes, mocks and compile checks
ctest --test-dir build/dev -L fuzz                  # fuzzers (FORGE_FUZZ_ITERS inputs each)
./build/dev/test_os                                 # one suite directly
FORGE_TEST_ONLY=emit ./build/dev/test_script        # one group of cases
```

Wine, the Android emulator and qemu: see [building.md](building.md). Results per platform: [STATUS.md](STATUS.md).

## CI
`.github/workflows/ci.yml` runs on every pull request, on pushes to `main`, and on manual dispatch. All jobs set `FORGE_THREADS=2`.

| Job | Runner | What it does |
|-----|--------|--------------|
| `linux-gcc`, `linux-clang` | ubuntu-24.04 | `dev` preset with g++-14 / clang-18 and ccache; `ctest -LE "slow|emu"` |
| `linux-vulkan` | ubuntu-24.04 | g++-14 with glslang, Vulkan headers and Mesa lavapipe; builds `forge` and `test_vulkan`, checks `test_vulkan` reports an `llvmpipe` device (a skip fails the job), then `ctest -R vulkan` (both `test_vulkan` runs and the two Vulkan goldens) |
| `linux-asan`, `linux-tsan` | ubuntu-24.04 | clang-18 with `FORGE_SANITIZE` (Debug) / `FORGE_TSAN`; builds and runs `test_script` only |
| `shells` | ubuntu-24.04 | Installs many shells; runs `FORGE_TEST_ONLY=emit test_script` |
| `macos` | macos-latest | `dev` preset; `ctest -LE "slow|emu"` |
| `ios` | macos-latest | Xcode device build of `forge_c`, `forge_mobile`, `forge_text` with `ios.cmake`; then a Ninja arm64 simulator build of `test_os`, `test_text`, `test_locale`, `test_mobile`, `test_util`, run in a booted iPhone simulator with `xcrun simctl spawn` (`.github/ios_sim_check.sh`) |
| `windows-msvc` | windows-latest | MSVC, Ninja; `ctest -LE "slow|emu"`, then the emitted cmd/pwsh/PowerShell goldens |
| `windows-mingw` | windows-latest | MSYS2 UCRT64 GCC; `ctest -LE "slow|emu"` |
| `freebsd` | FreeBSD 14.3 VM | Native build; `ctest -LE "slow|emu|gpu"` |
| `android` | ubuntu-24.04 + API 34 x86_64 emulator | NDK cross builds; `--emit android` goldens, `test_script`, `test_mobile`, `test_locale`, `test_text`, `test_os`, `test_os_ext`, `test_os_sys`, `test_util` and `test_loadbalancer` on the emulator (`.github/android_check.sh`) |

Only `linux-vulkan` installs `glslangValidator` and the Vulkan headers; every other job compiles the Vulkan backend and its tests out. To reproduce `linux-vulkan` locally, set `VK_ICD_FILENAMES` and `VK_DRIVER_FILES` to the lavapipe ICD (`/usr/share/vulkan/icd.d/lvp_icd*.json`) so the loader cannot pick a real GPU. `test_rpc_tls` skips on Windows CI (its `openssl` probe redirects to `/dev/null`). Pass counts per job: [STATUS.md](STATUS.md).

`ci/run-local.sh [job...]` mirrors these jobs with user-local toolchains for a machine without GitHub Actions. It hard-codes default toolchain paths under `/workspace` that you override with environment variables (`LLVM`, `MINGW_TC`, `WINE`, `ZIG`, `NDK`, `SHELL_DIRS`, ...). `ci/repro_check.sh` checks that two clean Release builds give bit-identical `forge`, `forge-serve` and `libforge.so`.

## Useful environment variables
- `FORGE_THREADS`: worker pool size (use 2 on small machines and emulators).
- `FORGE_SIMD_TARGET=scalar`: run with a specific kernel table.
- `FORGE_TEST_ONLY`: run a subset of cases.
- `FORGE_VK_SSBO_ALIGN=256`: emulate a strict Vulkan driver.
- `MOCK_DEVICES=N`, `MOCK_MEM_LIMIT=<bytes>`: devices and memory the mock GPU drivers report.
- `FORGE_UPDATE_EMIT_GOLDEN`, `FORGE_GOLDEN_UPDATE`: rewrite the emit and script golden files instead of comparing.

The full list is in [configuration.md](configuration.md).

## Fuzzing
By default each fuzz target links a small built-in mutation driver (`fuzz/driver.cpp`) and runs `FORGE_FUZZ_ITERS` inputs over its corpus in `fuzz/corpus/<name>/`, with a 600 s timeout. With clang and `-DFORGE_LIBFUZZER=ON` they link libFuzzer instead and are not registered with ctest. `fuzz/make_corpus.py` regenerates the seed inputs.

## Performance tools (not part of ctest)
- `tools/perf_baseline.py --forge build/release/forge --model <model.fpk> [--gemm N] [--reps R] [--baseline perf/baseline-x86_64.json] [--check] [--tolerance T]` runs `forge bench` and records or checks throughput against a stored baseline.
- `test_cuda_hip bench <codec> <rows> <cols> <k> [reps]` times one quantised matmul on CUDA device 0 (e.g. `q4_k 512 25600 5120`). Inputs stay resident inside a run, so only the kernels are timed; it reports the fastest of 7 batches of `reps` calls (default 20), the median, TOPS, weight GB/s and the kernel path (`qmatmul_mma`, `qmatmul_mmq` or `qmatmul_q81`).
- `tools/bench_llm.sh --gguf <model.gguf> [--fpk <model.fpk>] [--forge PATH] [--llama-bench PATH] [-p 512] [-n 128] [-r 3] [--device cuda] [--codec q4_k] [--nsys PREFIX] [--no-llama]` compares FORGE with llama.cpp's `llama-bench` on the same model: prefill and decode tok/s, FORGE with and without `FORGE_GRAPHS=1`, and FORGE's GPU driver calls per decode token from `forge bench`. `--nsys` adds an Nsight Systems kernel summary per FORGE run. It needs a GPU and a model.

## Golden test Python environment
`golden_tiny_qwen3_5` skips without torch and transformers 5 or newer. `golden_quant_inplace` skips its GGUF sub-check without `gguf`. Install the pinned set from `verify/golden/requirements.txt` into a venv and configure with `-DFORGE_GOLDEN_PYTHON=<venv>/bin/python`.
