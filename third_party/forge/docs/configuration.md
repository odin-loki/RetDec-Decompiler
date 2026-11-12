# Configuration: environment variables

FORGE has no configuration file. Programs take command-line flags ([cli.md](cli.md), [serve.md](serve.md), [rpc.md](rpc.md)) and read the environment variables below. The list was built by searching the sources for `getenv`. Most values are read once, the first time they are needed, so set them before the program starts.

## Devices and kernels

| Variable | Read by | Effect |
|----------|---------|--------|
| `FORGE_DEVICE` | `inference::model` (so `forge run`, `forge bench`, `forge-serve`, the C ABI and the bindings) | Execution device: `cpu` (default), `vulkan`, `cuda[:N]`, `hip[:N]`, `opencl[:N]`, `ze[:N]` / `level_zero[:N]`, `metal[:N]`, `rpc:host:port`, or a pipeline `pipe:<dev>[*weight],<dev>...` (e.g. `pipe:vulkan*3,cpu`). An unavailable device logs a warning and falls back to the CPU |
| `FORGE_SIMD_TARGET` | simd dispatch | Force a kernel table: `scalar`, `avx2`, `avx512`, `neon`, `sve`, `rvv`. Ignored if the table is not usable on this CPU. The old name `FORGE_LANE_TARGET` is still read |
| `FORGE_SVE_KEEP_VL=1` | simd (aarch64 Linux) | Do not change the SVE vector length at start-up when there is no kernel table for the hardware length |
| `FORGE_THREADS` | task pool | Worker thread count (default: physical cores this process may use on Linux, else hardware concurrency) |
| `FORGE_SPIN_US` | task pool | How long idle workers spin for the next `parallel_for` before sleeping (default 200; 0 = sleep at once) |
| `FORGE_PROCESSES` | `os::process_pool` | Worker process count when the pool is created with 0 workers (default: hardware concurrency) |
| `FORGE_CPU_INT8=0` | quantised matmul (CPU) | Turn off the int8-activation integer kernels (q8_0/q4_k/q5_k/q6_k) and use the f32 decode + dot path, the accuracy reference. `=1` forces them even with only the scalar kernels (default: on when a SIMD integer table exists) |
| `FORGE_QI8_TARGET` | quantised matmul (CPU) | Force an integer-kernel table: `scalar`, `avx2`, `avxvnni`, `avx512vnni` |
| `FORGE_QI8_PF` | quantised matmul (CPU) | Software prefetch distance of the integer kernels in bytes (default 2048, 0 = off) |
| `FORGE_QDOT=0` | quantised matmul (CPU) | Use the plain scalar decode + dot path instead of the SIMD one (benchmarking, bisecting) |

## GPU tuning and debugging

These switch individual GPU code paths. Most are for benchmarking and bisecting; the defaults are the tested configuration. [gpu.md](gpu.md) describes each kernel path they control.

| Variable | Applies to | Effect |
|----------|-----------|--------|
| `FORGE_GPU_NO_RESIDENCY` | CUDA, HIP, OpenCL, Level Zero, Metal | If set, tensors are not kept on the device between ops; every op uploads and downloads. Vulkan always keeps tensors resident |
| `FORGE_GPU_EAGER_SYNC` | CUDA, HIP | If set, every run ends by syncing all resident buffers to the host; turns off the fixed arena and whole-run replay |
| `FORGE_GPU_NO_STATE_OPS` | CUDA, HIP | If set, Gated DeltaNet state ops run on the host instead of the device |
| `FORGE_GPU_QMATMUL=0` | CUDA, HIP | Do not run quantised matmul on the device with encoded weights; weights are decoded to f32 within `FORGE_GPU_WEIGHT_BUDGET` instead |
| `FORGE_GPU_QGATHER=0` | CUDA, HIP | Do not gather from encoded embedding tables on the device |
| `FORGE_GPU_WEIGHT_BUDGET` | resident backends | f32 bytes of decoded quantised weights mirrored on the device, e.g. `4G`, `512M` (suffix `k`, `m`, `g`; default 8 GiB). The rest stay encoded and run on the CPU. Codecs with a device quantised matmul stay encoded on the device instead |
| `FORGE_GPU_ARENA_MB` | CUDA, HIP | Largest activation arena held at fixed device addresses as one buffer, in MiB (default 1024; 0 disables) |
| `FORGE_GPU_POOL_MB` | CUDA, HIP | MiB of freed device buffers kept for reuse (default 4096) |
| `FORGE_GPU_REPLAY` | CUDA | If set, enables experimental whole-run decode replay (relaunches recorded device work without the executor). Needs decode graphs (on unless `FORGE_GRAPHS=0`) and the fixed arena |
| `FORGE_REPLAY_VERIFY` | CUDA | With `FORGE_GPU_REPLAY`: do one real run every N runs (default 32) |
| `FORGE_GRAPHS=0` | CUDA | Do not capture decode kernel launches into CUDA graphs (capture and replay are on by default) |
| `FORGE_GRAPH_REPLAY=0` | CUDA | With decode graphs on: always re-capture instead of patching kernel parameters in place |
| `FORGE_GRAPH_SEG` | CUDA | With decode graphs on: kernels per captured segment (default 64; 0 = no splitting) |
| `FORGE_GRAPH_SEG0` | CUDA | Kernels in a run's first captured segment, doubling per segment up to `FORGE_GRAPH_SEG` (default 8; 0 = off) |
| `FORGE_REPLAY_EAGER=0` | CUDA | Whole-run replay patches every segment before launching any (default: patch and launch one at a time) |
| `FORGE_GPU_STAGE=0` | CUDA | Small host-to-device copies (≤ 4 KB) go from pageable memory instead of a page-locked staging ring |
| `FORGE_CUDA_SCHED` | CUDA | `spin`, `yield` or `block`: the context's host wait mode (default auto) |
| `FORGE_CUDA_IMAGE` | CUDA | Load only this embedded PTX image: `ptx` (sm_70) or `ptx sm_80`. By default `sm_80` is tried first, then `sm_70` |
| `FORGE_MMA=0` | CUDA | Disable the int8 tensor-core prefill matmul (`sm_80` image) and the q6_k/q8_0 weight repacking; prefill uses MMQ |
| `FORGE_MMA_BT` | CUDA | `64` or `128`: tokens per tensor-core matmul block (default: 64 for q4_k, 128 for q6_k/q8_0 when rows ≥ 128) |
| `FORGE_MMA_STREAMK` | CUDA | `1`: persistent stream-K tensor-core matmul for grids of 1/4 to 4 waves that do not fill whole waves (off by default); `2`: for every such grid, however small. Read per launch |
| `FORGE_MMA_SK_MIN` | CUDA | Minimum 256-value k chunks per stream-K block (default 4) |
| `FORGE_MMA_SPLITK=0` | CUDA | Turns off split-K for tensor-core matmuls whose grid cannot fill the GPU (k/v projections, DeltaNet a/b); on by default |
| `FORGE_MMA_STAGES=2` | CUDA | Double-buffered q4_k tensor-core matmul (cp.async copies the next k-chunk while one is computed); default one stage |
| `FORGE_CPU_ATTN` | CPU | Chunked GQA-shared attention (`graph/attn_cpu.hpp`): by default used with an int8 KV cache only; `new` uses it for f32 KV too (its f32 prefill has not yet been shown faster), `old` never |
| `FORGE_CPU_DELTA` | CPU | `old` selects the previous two-pass delta_rule instead of the column-blocked one-pass kernel |
| `FORGE_OP_TIME` | CPU executor | `1`: wall time per op kind (attention split by KV codec, state ops by fn), printed at exit |
| `FORGE_DELTA_RULE_OLD` | CUDA | If set, Gated DeltaNet uses the original `forge_delta_rule` kernel instead of the register-state ones |
| `FORGE_DELTA_RULE_R` | CUDA | If set, Gated DeltaNet uses the earlier register-state kernels `forge_delta_rule_r8/_r16/_r32` (a block per head and 32 columns) instead of the staged `_p8/_p16/_p32` |
| `FORGE_CAUSAL_CONV_OLD` | CUDA, HIP | If set, the DeltaNet causal conv uses the per-channel `forge_causal_conv` kernel instead of the token-parallel `forge_causal_conv_t` |
| `FORGE_MMVQ=0` | CUDA, HIP | Disable the q8_1/dp4a decode matmul kernels |
| `FORGE_MMVQ_CPB` | CUDA, HIP | `1`, `2` or `4`: output columns per decode-matmul block (default: 2 for q4_k with k ≤ 6144, else 1) |
| `FORGE_MMVQ_Q8V=0` | CUDA, HIP | q8_0 decode matmul uses the older 4-bytes-per-lane loop |
| `FORGE_MMVQ_NC=1` | CUDA | Decode-matmul weight loads bypass L1 (`ld.global.nc.L1::no_allocate`); measured no faster on an RTX 3090 |
| `FORGE_MMQ_MIN_ROWS` | CUDA, HIP | Rows at or above which quantised matmul uses the tiled MMA / MMQ kernels (default 16; 0 disables both) |
| `FORGE_ATTN_DEC=0` | CUDA, HIP | Disable split-K decode attention |
| `FORGE_GPU_NO_FA` | CUDA, HIP | If set, prefill attention does not use the flash-attention kernels (including the tiled `sm_80` ones) |
| `FORGE_FA_TC=0` | CUDA | Prefill attention on the `sm_80` image uses the f32 tiled kernel instead of the fp16 tensor-core FlashAttention-2 kernel (`forge_attention_fa2tc_*`; Q/K/V/P rounded to fp16, f32 accumulation) |
| `FORGE_FUSE_Q81=0` | CUDA, HIP | Do not fuse q8_1 activation quantisation into decode `rms_norm` / `silu_mul` / the attention combine pass |
| `FORGE_FUSE_Q8R=0` | CUDA | Prefill `rms_norm` does not write the int8 activation rows for the tensor-core matmul, and consecutive tensor-core matmuls on the same activation (q/k/v, gate/up) each quantise it again |
| `FORGE_FUSE_ADD_NORM=0` | CUDA | Do not run a residual `add` and the `rms_norm` of its result as one kernel (`forge_rms_norm_f`) |
| `FORGE_FUSE_QK_ROPE=0` | CUDA | Do not run a per-head `rms_norm` and the `rope` that consumes it (Qwen3 QK-norm) as one kernel (`forge_rms_rope`) |
| `FORGE_ROPE_HPB` | CUDA | Heads per block of `forge_rms_rope` (default: 16 at 32+ tokens, else 256 / head dim); tuning only |
| `FORGE_FUSE_GLU=0` | CUDA | Do not fuse the FFN gate and up projections and their SiLU product into one step (`silu_mul(linear(h, gate), linear(h, up))`); each gate/up weight pair is then uploaded as two weights instead of one interleaved one. Read when a plan is compiled |
| `FORGE_PLAN_TRACE` | all devices | If set, print at exit the ops (with shapes and counts) that ran on the host during device runs |
| `FORGE_VK_DEBUG` | Vulkan | If set, log why an op was not offloaded to Vulkan |
| `FORGE_VK_SSBO_ALIGN=N` | Vulkan | Emulate a stricter storage-buffer offset alignment (testing) |
| `FORGE_VK_PIPELINE_CACHE=0` | Vulkan | Disable the pipeline cache |
| `FORGE_CACHE_DIR` | Vulkan | Pipeline cache directory (else `$XDG_CACHE_HOME/forge`, else `~/.cache/forge`) |
| `FORGE_OPENCL_TYPE` | OpenCL | Devices to enumerate: `gpu`, `cpu` or `all` (default; GPUs listed first) |

## Sessions and memory

See [memory.md](memory.md) for how the budget is used.

| Variable | Effect |
|----------|--------|
| `FORGE_KV_CACHE_TYPE`, `FORGE_KV_CODEC` | KV cache codec when `session_options.kv_codec` is empty (`FORGE_KV_CACHE_TYPE` is checked first): `f32` (default), `int8`, `fp8`, `tbq4`, `tbq3`, `tbq4p`, `tbq3p`, or `k8v4` (int8 keys, tbq4 values) |
| `FORGE_KV_OFFLOAD=1` | Keep the KV cache on the host and drop device copies after each attention (when `session_options.kv_offload` is unset) |
| `FORGE_DECODE_WEIGHTS=1` | Decode every weight to f32 at load time, instead of reading quantised weights in place from the mapped file |
| `FORGE_HUGEPAGES=1` | Advise huge pages for memory mappings of 2 MiB or more |
| `FORGE_MEM_LIMIT` | Host memory budget: `6G`, `512M`, `1.5GiB`, bytes, or `75%` of the detected limit; `0` / `off` disables. Default: `FORGE_MEM_FRACTION` of min(RAM, cgroup limit, `RLIMIT_AS`) |
| `FORGE_MEM_FRACTION` | Share of the detected limit used when `FORGE_MEM_LIMIT` is unset (default `0.85`) |
| `FORGE_MEM_SOFT` | Soft watermark as a share of the limit (default `0.9`) |
| `FORGE_MEM_DEVICE_LIMIT` | Limit for device memory pools (default: accounting only) |
| `FORGE_MEM_PRESSURE=0` | Ignore OS memory-pressure signals |
| `FORGE_MEM_SPILL_DIR`, `FORGE_MEM_SPILL_MAX` | Spill-store directory (default `<tmp>/forge-spill-<pid>`) and size cap |

## Models and security

| Variable | Effect |
|----------|--------|
| `FORGE_MODEL_KEY`, `FORGE_MODEL_KEY_FILE` | Key (64 hex digits) or key file for encrypted `.fpk` files. `FORGE_MODEL_KEY` wins if both are set |
| `FORGE_REQUIRE_SIGNED=1` | Refuse models without a valid signature |
| `FORGE_TRUSTED_KEYS=<hex>[,<hex>]` | Only accept signatures from these Ed25519 public keys (implies a signature is required) |
| `FORGE_SECURITY_POLICY` | Policy file for `forge::security::load_policy` and `forge security check` (an error if it cannot be read). See [security-api.md](security-api.md) |
| `FORGE_SECURITY_PROFILE` | Base FSS profile: `strict`, `standard` or `permissive` (audited). Without it and without a policy file no FSS profile is active |
| `FORGE_CACHE` | Where `forge run` / `forge bench` cache models imported on the fly (default `~/.cache/forge`) |
| `FORGE_TEMPLATES` | Directory of import blueprint templates (default: `import/templates` in the source tree the binary was built from) |

## RPC clients

Read by `rpc:host:port` devices and `forge::remote` (see [rpc.md](rpc.md)).

| Variable | Effect |
|----------|--------|
| `FORGE_RPC_TOKEN` | Bearer token |
| `FORGE_RPC_KEY` | Secret key file for key authentication |
| `FORGE_RPC_TLS=1` | Use TLS (also implied by any of the next three variables) |
| `FORGE_RPC_CA` | CA file used to verify the server certificate |
| `FORGE_RPC_FINGERPRINT` | SHA-256 fingerprint of the server certificate |
| `FORGE_RPC_TLS_CERT`, `FORGE_RPC_TLS_KEY` | Client certificate and key, for servers started with `--tls-client-ca` |
| `FORGE_RPC_SERVER_NAME` | Server name for TLS verification |

`forge rpc-serve --allow-scripts` removes `FORGE_SCRIPT_CACHE_SIGN_SEED`, `FORGE_RPC_TOKEN`, `FORGE_RPC_KEY` and `FORGE_RPC_TLS_KEY` from the environment of script child processes.

## Scripting

| Variable | Effect |
|----------|--------|
| `FORGE_SCRIPT_PATH` | Extra module search directories for script imports (separated by `:` or `;`; `;` only on Windows) |
| `FORGE_SCRIPT_CACHE` | Directory for the compiled-script cache (unless `--cache-dir` is given) |
| `FORGE_SCRIPT_CACHE_SIGN_SEED` | 64-hex Ed25519 seed used to sign cache entries (unless `--cache-sign-seed` is given) |
| `FORGE_SHELL` | Shell used by `forge::host_shell()` and `forge script --emit auto` |
| `FORGE_ASK_MODEL` | `.fpk` that `forge::ask` and related calls run in-process (unless `--ask-model` is given) |
| `FORGE_ASK_MAX_TOKENS` | Reply cap for those calls (default 256) |
| `FORGE_AI_ENDPOINT` | OpenAI-compatible endpoint used when no in-process model is set, e.g. `http://127.0.0.1:8080` from `forge-serve` |
| `FORGE_AI_MODEL` | `model` field sent to that endpoint (default `default`) |
| `FORGE_AI_KEY` | Sent as `Authorization: Bearer <key>` to that endpoint |

## Logging and console

| Variable | Effect |
|----------|--------|
| `FORGE_LOG` | Global and per-module thresholds, e.g. `info` or `warn,graph=debug`. Levels are matched by first letter: `trace`, `debug`, `info`, `warn` (default), `error`, `fatal`, `off` |
| `FORGE_LOG_FORMAT=json` | JSON-lines log output |
| `NO_COLOR`, `FORCE_COLOR`, `CLICOLOR_FORCE=1`, `TERM=dumb` | Colour on or off for `os::console` output |

## OS layer

| Variable | Effect |
|----------|--------|
| `FORGE_AIO=threads` | Force the thread-pool backend for async file I/O instead of io_uring, IOCP or kqueue |
| `FORGE_KEYSTORE=file` | Force the file keystore |
| `FORGE_KEYSTORE_DIR` | File keystore directory (default `$XDG_DATA_HOME/forge/keys`, `%LOCALAPPDATA%\forge\keys` on Windows; required on Android) |
| `FORGE_MOBILE_TRACE` | Replay a JSON trace file instead of opening the real device in `os::mobile` |
| `FORGE_MOBILE_SYSFS`, `FORGE_MOBILE_INPUT` | Linux `os::mobile`: roots used instead of `/sys` and `/dev/input` |
| `NOTIFY_SOCKET`, `WATCHDOG_USEC`, `INVOCATION_ID`, `JOURNAL_STREAM` | Standard systemd variables, used by `os::service` |
| `TZ`, `TZDIR` | Time zone and zoneinfo directory, used by `os::time` and `forge::os::locale` |

## Bindings
- `FORGE_LIB`: path to `libforge` for the Python binding (tried first). Treat it as trusted configuration.
- The Android example and the Swift binding set `FORGE_DEVICE` themselves.

## Test-only
Used by the test suites, mock drivers and test scripts; not by the library:

| Variable | Used by |
|----------|---------|
| `FORGE_TEST_ONLY` | Every `test_*` binary: run only cases whose name contains the value |
| `FORGE_OS_EXT_CHILD` | `test_os_ext` child-process mode. **Do not export it in a shell** |
| `FORGE_OS_T`, `FORGE_TEST_DYNLIB_PATH`, `FORGE_TEST_LOG_EXIT_FILE`, `FORGE_TST_*`, `FORGE_EMIT_X`, `FORGE_EMIT_NOPE` | Set and read by the tests themselves |
| `FORGE_TEST_H264` | `test_video` input file |
| `FORGE_TEST_PLUGIN_PATH`, `FORGE_TEST_SHELL_PATH` | `test_script`: plugin to load, extra shell directories |
| `FORGE_UPDATE_EMIT_GOLDEN`, `FORGE_GOLDEN_UPDATE` | Rewrite emit and script goldens instead of comparing |
| `FORGE_UCD_DIR` | `test_text_conformance` data directory (default `tests/data/ucd`) |
| `FORGE_ZE_TEST_IMAGE`, `MOCK_DEVICES`, `MOCK_ALIGN`, `MOCK_MEM_LIMIT` | GPU tests and mock drivers |
| `FORGE_OPENCL_TEST_ALIGN` | The OpenCL runtime: overrides its buffer alignment, for tests |
| `FORGE_RUNNER` | Golden scripts: emulator prefix when cross-compiling |
| `FORGE_TEST_FPK` | Android and Swift binding tests |
| `FORGE_TEST_TIMEOUT` | `tests/android_emulator.sh` per-test limit (default 600 s) |
| `FORGE_MOBILE_BUILD_DIR` | `tests/mobile_build.sh` build directory |
