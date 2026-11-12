# FORGE test status

What has been built and tested where. Current as of 2026-11-04, main at ccc0a7d (PR #53).
CI is `.github/workflows/ci.yml`; it runs on every pull request and every push to main.
"Skip" means a test exited 77 because its device or tool was missing.

## Platforms

| Platform | Built | Tested | How |
|---|---|---|---|
| Linux x86_64, GCC 14 | CI `linux-gcc` | ctest `-LE "slow\|emu"`: 111/111 pass (GPU device tests skip) | Ubuntu 24.04, `cmake --preset dev` |
| Linux x86_64, Clang 18 | CI `linux-clang` | same, 111/111 | Ubuntu 24.04 |
| Linux, ASan + UBSan | CI `linux-asan` | `test_script` only | Clang 18, `FORGE_SANITIZE=ON` |
| Linux, TSan | CI `linux-tsan` | `test_script` only | Clang 18, `FORGE_TSAN=ON` |
| Linux, Vulkan on lavapipe | CI `linux-vulkan` | `test_vulkan`, `test_vulkan_align256`, `golden_tiny_llama_vulkan`, `golden_moe_mla_vulkan` (added 2026-11-05; first CI run pending) | GCC 14, Ubuntu `glslang-tools` + `libvulkan-dev` + `mesa-vulkan-drivers`; loader pinned to the lavapipe ICD |
| macOS | CI `macos` | ctest 111/111, including `test_gpu_metal` | `macos-latest` runner, Xcode toolchain |
| iOS | CI `ios` | `test_os`, `test_text`, `test_locale`, `test_mobile`, `test_util` in an arm64 iPhone simulator via `xcrun simctl spawn` (added 2026-11-05; first CI run pending) | `forge_c`, `forge_mobile`, `forge_text` built with Xcode for device, signing off; tests built with Ninja for the simulator (`FORGE_IOS_SIMULATOR=ON`, arm64) |
| Windows, MSVC | CI `windows-msvc` | ctest 106/106; script emit goldens | `windows-latest`, Ninja + `cl` |
| Windows, MinGW | CI `windows-mingw` | ctest: 84 run, all pass | MSYS2 UCRT64 GCC |
| FreeBSD 14.3 | CI `freebsd` | ctest `-LE "slow\|emu\|gpu"`: 60/60 | `vmactions/freebsd-vm` VM on Ubuntu |
| Android arm64-v8a | CI `android` | build only | NDK, `tests/mobile_build.sh` |
| Android x86_64 | CI `android` | `test_script`, `--emit android` goldens, `test_mobile`, `test_locale`, `test_text`, `test_os`, `test_os_ext`, `test_os_sys`, `test_util`, `test_loadbalancer` on an API 34 emulator (the last five added 2026-11-05; first CI run pending) | NDK build of `forge`, `test_script`, the example plugin; KVM emulator |
| Android, physical device | no | never | — |
| Linux aarch64 (SVE) | locally, zig cross | under qemu-user only (2026-10-31: 50/54, see changelog) | `qemu-aarch64 -cpu max,sveN=on` |
| Linux riscv64 (RVV) | locally, zig cross | under qemu-user only (same) | `qemu-riscv64 -cpu rv64,v=true,vlen=N` |
| Windows under wine | locally, mingw cross | historical full-suite passes (2026-10-29/31) | `tests/wine_run.sh` |

Windows emit goldens (`FORGE_TEST_ONLY=emit test_script`, `windows-msvc`): sh, bash, dash, pwsh and PowerShell 5.1
for all three goldens; cmd for `batch` (the other two are outside the cmd subset and refused).

Shell emit goldens (`shells` job, 2026-11-04 run): only sh, bash, dash, ash (busybox), pwsh and powershell were
found and tested. The other shells in the job's install list were not present in that run.

## GPU backends

| Backend | Real hardware | Mock / software | CI |
|---|---|---|---|
| CPU | every platform above | — | all jobs |
| CUDA | NVIDIA GeForce RTX 3090: WSL2 (2026-10-31, 2026-11-05) and native Windows 11, CUDA 13.2 driver (2026-11-04) | mock driver (`test_gpu_*cuda_mock`) | mocks pass on `linux-gcc`/`linux-clang`; skip on macOS and Windows |
| HIP | **never** | mock driver | mocks pass on Linux |
| OpenCL | **never** | mock driver runs the generated OpenCL C as C | mocks pass on Linux |
| Level Zero | **never** | mock driver | mocks pass on Linux and `windows-msvc` |
| Metal | GitHub `macos-latest` runner's Metal device: `test_gpu_metal` passes (oracle check of every `metal.*` lowering) | MSL host syntax check | `macos` |
| Vulkan | **never** on a GPU | Mesa lavapipe (local 2026-11-05: `test_vulkan` 33 kernels, 0 failures, both Vulkan goldens pass on llvmpipe); SwiftShader in the Android emulator (local) | `linux-vulkan` on lavapipe; compiled out in every other job |

`test_gpu_metal` passing means the runtime opened a device and all checks passed; the device name is not in the
CI log because ctest only prints output on failure.

### CUDA on RTX 3090, Windows 11 (2026-11-04)

`test_cuda_hip` cross-built with mingw-w64 and run natively:

| Check | Result |
|---|---|
| CUDA lowerings vs the float64 oracle | 31/31 |
| Quantised `matmul_t` cases | 24/24 |
| `delta_rule`, `causal_conv` state ops | match the reference |
| Split-K decode attention shapes | 7/7 |

HIP on that machine: mock only (no ROCm runtime).

### CUDA performance on RTX 3090 (2026-11-05)

Idle RTX 3090, Windows 11 host, WSL2 Ubuntu, clang 23, CUDA 13 driver 610.88. 512-token prefill and 128-token
decode, tok/s. llama.cpp is `llama-bench` at bc428e4 on the same GGUF file; FORGE imports that GGUF to `.fpk`.
"Before" is main before the `cuda-mma-prefill` branch.

| Model (Q4_K_M) | Prefill before → after | llama.cpp prefill | Decode before → after | llama.cpp decode |
|---|---|---|---|---|
| Qwen3.5 9B | 326 → 2720 | 4740 | 75.6 → 107.5 | 120.5 |
| Goedel-Prover-V2 32B (Qwen3) | 88 → 839 | 1360 | 26.2 → 35.3 | 38.4 |

Greedy output is identical with CUDA graphs on and off.

## CPU quantised matmul (2026-11-06, Xeon Gold 6242 x2, WSL2, 32 threads, machine shared with GPU jobs: noisy)
- `forge bench --kernels 1024` (1 GB matrix, k=4096), int8 avx512vnni vs f32 decode+dot: q4_k 27.1 vs 17.9 GB/s, q5_k 28.9 vs 19.5, q6_k 30.3 vs 21.5, q8_0 24.8 vs 21.2. Two vectors cost the same as one; eight run at 19–25 GB/s of weights (8x the work).
- q35 (Qwen3.5 9B Q4_K_M) `bench -p 16 -n 8`, same build and load: FORGE_CPU_INT8=0 prefill 4.4 / decode 0.6 tok/s, int8 8.0–8.2 / 1.1–1.4 tok/s. An earlier quiet-machine baseline (old build) was prefill 16.5 / decode 1.8 tok/s at -p 128; the load on the machine made later absolute numbers incomparable. Greedy tokens on the test prompt are identical to the f32 path.
- Not done: §2 interleaved prefill tiles, multi-product dispatch, per-token serial work on the pool, NEON/SVE integer kernels (aarch64 keeps the f32 path by default).

## Known gaps

- iOS: CI runs five test executables in the simulator only; never run on a device. The Swift package and XCTest have
  not been run.
- Android: never run on a physical device. CI runs `test_script` and eight other suites on the x86_64 emulator; the
  rest of the suite (`test_inference` and other heavy tests) is not run there.
- HIP, OpenCL and Level Zero have never run on real hardware. Vulkan has never run on a real GPU.
- CUDA: one GPU model (RTX 3090). No multi-GPU or peer-copy test on hardware.
- Vulkan is built and tested in CI only on lavapipe (`linux-vulkan`); every other job compiles it out.
- `test_rpc_tls` on Windows CI: the `openssl` CLI probe no longer uses a shell redirect, but it has not yet run there.
- AMX/SME: AMX ran natively and under Intel SDE; SME only under qemu.
- GPU memory: no eviction while a CUDA/HIP stream capture or an OpenCL/Level Zero graph recording is open;
  graphs recorded before an eviction must not be replayed after it.
- RPC: permissions are coarse (`device`, `scripts`); client-certificate rules match only the fingerprint or subject CN.
- Not tested anywhere: hybrid P/E cores, multi-node NUMA, batteries and thermal sensors, running under
  systemd or the Windows SCM, macOS kperf, VA-API/NVDEC/MediaCodec/VideoToolbox decode, CUDA graphics interop.
- Security gaps: [security.md](security.md#known-gaps).

## Changelog of checks

Newest first. Results are from the date given; later code may differ.

### 2026-11-05 (branch `cuda-mma-prefill`, RTX 3090)
- CUDA prefill and decode speedups; numbers in [CUDA performance](#cuda-performance-on-rtx-3090-2026-11-05).
  A second PTX image for `sm_80` (tried first, `sm_70` fallback) carries int8 tensor-core prefill matmul for
  q8_0/q4_k/q6_k and tiled prefill attention. Gated DeltaNet keeps its state in registers. Decode MMVQ uses a
  4-warp block per output column, with q6_k padded to 224-byte blocks and q8_0 regrouped on upload. CUDA graph
  capture is now on by default (`FORGE_GRAPHS=0` turns it off). Less host work per decode token: no device fetch
  before overwriting an input, no sync after uploads, logits read in place, plain greedy takes the argmax directly.
- Fixed: a destroyed plan left its pinned constants resident on the device, so a later buffer at the same host
  address could find a stale entry; `~plan` now forgets them and `forget` drops their pin records.
- Full Linux ctest passes on that machine, including the CUDA/HIP mock goldens and the tiny Qwen3.5 golden
  against transformers. `test_cuda_hip` adds prefill attention shapes, three delta_rule shapes, an MMA-aware
  quantised matmul reference and an `sm_80` image check.

### 2026-11-04
- PR #53 merged as ccc0a7d. Its last PR run (41802863931) passed all 11 CI jobs: linux-gcc, linux-clang,
  linux-asan, linux-tsan, macos, ios, freebsd, android, windows-msvc, windows-mingw, shells.
- CI now covers native macOS, iOS (build), Windows MSVC and MinGW, and FreeBSD in a VM. This supersedes
  older notes that macOS was only cross-linked, iOS never built, and the Windows event log sink and
  `os/dynlib.hpp` Windows/Apple code never compiled: `test_log` (including `eventlog_sink` on Windows) and
  `test_os_dynlib` pass on macOS, both Windows jobs and FreeBSD.
- CUDA on an RTX 3090 under Windows 11 (results above).
- Fixed: split-K decode attention launched its combine kernel with dangling argument pointers (7fe8a83).
- Fixed: on Windows, `find_executable` let an extensionless file on PATH shadow `cmd.exe` (1dea828); this was the
  `windows-mingw` `limit_counts.fs` failure.
- Fixed: Windows `ProcessPool` workers re-ran the whole test suite, an unbounded chain of test processes
  (d6377d3).
- Also: `strtod` fallback where libc++ lacks floating `from_chars` (FreeBSD 14, f6f1d11); CRLF-tolerant emit text
  goldens (8a90508); Metal build fixes after the module rename (f17c83d, d7d423d).
- Modules renamed: lane→simd, grid→gpu, flow→graph, pack→modelfile, mind→inference, balance→loadbalancer,
  spec→ops. `FORGE_LANE_TARGET` still works as an alias of `FORGE_SIMD_TARGET`.
- Added with tests: async and Windows event log sinks, `FORGE_LOG_MIN_LEVEL` (`test_log`, `log_min_level_cmake`);
  `limit {}` blocks and `allocs`/`handles`/`procs` limits; capability escape tests, which closed three gaps
  (`std::getenv` ignored `env`; pre-opened files and processes were usable in a narrower sandbox); RPC mTLS,
  channel binding and `allow=` permissions (`test_rpc`, `test_rpc_tls`; clients built before this cannot
  authenticate to a TLS server); `forge::remote_call`; GPU memory pools with LRU eviction
  (`test_gpu_memory_*_mock`); prefix KV spill (bit-identical logits); `os/dynlib.hpp`.
- PR #50: merges to main are gated on all CI checks.

### 2026-11-01 to 2026-11-03 (CUDA decode work, RTX 3090)
PRs #33–#48 moved CUDA decode onto the device. Decode speed reported in the PR titles: 1.2 → 4.5 tok/s at
p=1024 (#33), 4.8 → 17 (#35), 17 → 24 (#36), 24 → 29 with CUDA graphs (#38), split-K decode attention +29% (#48).
PR #49 (whole-run decode replay) gave no gain; PR #52 made it opt-in.

### 2026-10-31
- PR #18: CUDA on an RTX 3090 under WSL2, clang 23.1.4: ctest 104/104, CUDA tests on the device. Fixes: graph
  capture uses relaxed mode (global mode fails with error 900 on a real driver).
- PR #17: first GitHub Actions workflow.
- qemu-user, zig 0.17 musl static builds:
  - aarch64 (`-cpu max,sve256=on`) and riscv64 (`-cpu rv64,v=true,vlen=256`): `ctest -E '^(xos_|mobile_)'` 50/54
    with 6 GPU skips on both. All simd, SVE/RVV VL and fuzz tests pass. The 4 failures come from qemu-user (no
    dlopen in static builds, no binfmt for self-exec, `/proc/self` RSS 0) in `test_os`, `test_os_ext`,
    `test_os_process`, `test_script` (plugin).
  - Fixed: riscv64 has no `__NR_renameat` (cff8244).
- `ctest -L cross` (zig compile + link for FreeBSD, macOS, MinGW, musl): 39/39 after compiling each file with `-c`
  (ee0100c).
- QEMU TCG VMs: FreeBSD 14.5 amd64, all real tests pass after fixes; wine 11.20 (llvm-mingw build) every test
  passes, GPU tests skip. Bugs fixed then include kqueue missing in-place writes, a safetensors header-length
  overflow, Winsock `shutdown()` not waking `accept()`, and several cmd emitter errors.
- Shell emit goldens run on: sh, bash, zsh, dash, ash/busybox, yash, fish, tcsh, csh, nu, xonsh, elvish, pwsh,
  powershell (via pwsh), ksh, osh, ysh, hush, mksh, oksh, rc, es, and cmd under wine. Not tested: posh.
- `forge_mobile`: `test_mobile` 13/13 on x86_64 Linux (simulator, fake sysfs). Windows, FreeBSD, macOS, musl and
  Android backends compile-checked only; `apple.mm` not compiled then (now built in the `macos` job).

### 2026-10-30
- Linux dev builds: ctest 73/73 (zig), 76/76 (g++), 84/84 (clang 18, after scripting milestones 6–7). ASan and
  TSan runs of `test_script` and `test_rpc` clean.
- Android x86_64 emulator (API 34, TCG): 23 pass, 2 skip, 1 fail (`test_os_ext` socket binds; fix 7b7602c
  restored after 21c58e0 reverted it). The Android rerun after the fix was not done.
- wine: 32 pass, `test_vulkan` skips; one `test_os` timing flake fixed.
- `forge::os` extension, 16 features (`test_os_ext`): all pass on Linux and the Android emulator; under wine all
  pass except sandbox exec (wine ignores job limits).
- `test_vulkan` on SwiftShader: fixed a real misaligned-sub-buffer bug; lavapipe and the emulator now batch 571
  dispatches into 63 submits. `test_vulkan_align256` covers it.

### 2026-10-29 and earlier
- 8-core Xeon (AVX-512, AMX), no GPU. Vulkan on lavapipe: every kernel within 3e-7 relative of the reference;
  both goldens end-to-end on Vulkan.
- SVE at VL 128/256/512 and RVV at VLEN 128–1024 under qemu: simd tests pass at every length.
- AMX intermittent mismatches traced to the VM losing tile registers across context switches
  (`tools/amx_state_probe.cpp`); after fences and retries, 0/300 native and 0/10 SDE failures.
- SME f32 outer products pass under qemu at SVL 128–2048.
- KV-cache quantisation on the tiny model: max logit error vs f32 KV 6.8e-3 (int8), 3.1e-2 (fp8); Vulkan int8
  matches CPU int8 to 4e-7.
- Windows under wine (pure win64 prefix): 19 tests, 5 examples, 8 fuzzers pass. Real Windows bugs fixed then:
  IOCP on sync handles, unfused mingw `fmaf` (now `forge::fmaf_exact`), `std::rename` not replacing, mappings
  leaked when constructors threw.
- Android emulator: `test_os`, `test_loadbalancer`, `test_os_ext` pass; `test_inference` too heavy for TCG.
  Kotlin sources compiled with kotlinc 2.0.21.
- Text and locale: `test_text` 115/115, `test_locale` 223/223, UCD conformance 100% on 8 suites.
- `test_vulkan` parallel-run flake (shared `/tmp` paths) fixed with per-pid temp names.
