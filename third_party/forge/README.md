# FORGE

FORGE is a C++23 library and toolset for running AI models on CPUs and GPUs. One codebase covers:

- **SIMD kernels** chosen at run time for the CPU in use (`simd`)
- **GPU compute** behind one API, with runtimes loaded at run time: Vulkan, CUDA, HIP, OpenCL, Level Zero and Metal (`gpu`)
- **Inference** from a self-describing model file, `.fpk`, which carries its own computation graph (`graph`, `modelfile`, `inference`)
- **Serving**: an OpenAI-compatible HTTP server with a load-balancing proxy (`forge-serve`, `loadbalancer`)
- **A portable OS layer** for Linux, Android, Windows, macOS, iOS and FreeBSD (`os`)
- **Scripting**: `forge script`, an interpreted, memory-safe subset of C++23 that can also be translated to shell scripts (`script`)

Because the model file describes its own graph, a new architecture needs an import template, not new engine code. FORGE is a clean-room design, not a fork or wrapper of another project.

> **Status: in development (version 0.1.0).** CI builds and tests every pull request. On PR #53: Linux GCC 14 and Clang 18 111/111, macOS 111/111, Windows MSVC 106/106, Windows MinGW 84/84 run, FreeBSD 14.3 (VM) 60/60; ASan and TSan runs of the script tests; Android arm64-v8a and x86_64 cross-builds, with the script tests run on an x86_64 emulator. The iOS job only builds `libforge`; nothing has run on iOS or on a physical Android device. aarch64 and riscv64 have run under qemu only. GPUs: CUDA has run on a real RTX 3090 (WSL2 and native Windows 11), where the `test_cuda_hip` oracle and kernel checks pass. Metal's `test_gpu_metal` passes on the GitHub macOS runner's GPU. HIP, OpenCL and Level Zero have only run against mock drivers. Vulkan has run only on software devices (lavapipe, SwiftShader) and is compiled out in CI. Details: [docs/STATUS.md](docs/STATUS.md).

## Quick start (Linux)

You need CMake 3.25 or newer, Ninja, Python 3 and a C++23 compiler (CI uses g++-14 and clang-18).

```sh
cmake --preset dev                      # configure into build/dev (RelWithDebInfo)
cmake --build build/dev -j4
./build/dev/forge info                  # CPU features, simd target, GPU runtimes found, memory budget
ctest --test-dir build/dev -j4 -LE "slow|emu"   # tests; also writes build/dev/golden/tiny_f32.fpk
./build/dev/forge run build/dev/golden/tiny_f32.fpk --prompt "Hello" -n 8
```

The golden test model has random weights, so its output text is meaningless; it shows the pipeline runs end to end. The golden-model tests, and the tests that use the golden model, need the Python packages `numpy` and `safetensors` (`onnx` for `onnx_import`, `cryptography` for `modelfile_signing`). Without them CMake marks those tests "Not Run (Disabled)". Windows, macOS, FreeBSD and cross builds: [docs/building.md](docs/building.md).

To run a real model, import it first, or pass a Hugging Face directory, `.gguf` or `.onnx` file straight to `forge run` (it is imported once into `~/.cache/forge`):

```sh
./build/dev/forge import /path/to/hf_model -o model.fpk --codec q8_0
./build/dev/forge run model.fpk --prompt "Hello" -n 32
./build/dev/forge-serve model.fpk --port 8080
```

## What is in the box

| Layer | Headers | Provides |
|-------|---------|----------|
| core | `core/include/forge/core` | Tensors, dtypes (FP4/FP6/FP8, bf16, complex), errors, logging, memory budget, memory-mapped files |
| os | `os/include/forge/os` | Files, processes, process pools, sockets, IPC, NUMA, huge pages, CPU topology, sandboxing, keystore, services, dynamic libraries, text and locale (`forge_text`), mobile sensors (`forge_mobile`) |
| ops | `ops/` | One declaration per tensor op (`ops/defs/*.fspec`), a scalar reference, and the code generators |
| simd | `simd/include/forge/simd` | CPU feature detection and kernel tables: scalar, AVX2, AVX-512, NEON, SVE, RVV; AMX/SME tile kernels |
| gpu | `gpu/include/forge/gpu` | Devices, recorded graphs, parallel primitives, GPU runtimes loaded with `dlopen` |
| graph | `graph/include/forge/graph` | Graph IR, optimisation passes, memory planning, weight and KV codecs, autodiff, remote (RPC) devices |
| modelfile | `modelfile/include/forge/modelfile`, `modelfile/include/forge/import` | The `.fpk` container (BLAKE3, Ed25519 signing, XChaCha20-Poly1305 encryption), BPE tokenizer, importers for Hugging Face safetensors, GGUF and ONNX |
| inference | `inference/include/forge/inference`, `inference/include/forge.h` | Sessions, samplers, continuous batching, speculative decoding, GBNF grammars, JSON Schema, LoRA, diffusion and streaming sessions, the C ABI |
| loadbalancer | `loadbalancer/include/forge/loadbalancer` | Load-balancing pool (7 strategies, circuit breakers, retries, backpressure) and L4/L7 proxy |
| script | `script/include/forge/script` | The `forge script` interpreter, bytecode VM, static checker and shell emitters (`forge_script`) |
| support | `support/include/forge/*` | Hashing, JSON, async I/O, thread pool, caches, linear algebra, FFT/audio, vision, profiler, RNGs |
| verify | `verify/` | The oracle that checks every lowering against the op reference, and the golden-model scripts |

Programs built by default (`FORGE_BUILD_APPS=ON`):

- `forge`: command-line tool (`info`, `import`, `inspect`, `run`, `bench`, `dump`, `keygen`, `sign`, `verify`, `encrypt`, `rpc-serve`, `script`). See [docs/cli.md](docs/cli.md).
- `forge-serve`: OpenAI-compatible HTTP server, and `forge-serve proxy` load balancer. See [docs/serve.md](docs/serve.md).
- `libforge` (`forge_c` target; `libforge.so.1` on Linux, `forge.dll` on Windows): the C ABI used by the Python, Kotlin and Swift bindings.

## Repository layout

```
core/ os/ ops/ simd/ gpu/ graph/ modelfile/ inference/ loadbalancer/ script/ support/ verify/   library layers
apps/                  forge and forge-serve
import/templates/      blueprint templates used by the importers (llama, mixtral, deepseek_v2, qwen3, qwen3_5)
python/ bindings/      Python (ctypes), Android (Kotlin + JNI) and Apple (Swift) bindings
examples/              embedding examples (integrations/, built as example_*), script examples, a script plugin
tests/ fuzz/           unit, differential, script and fuzz tests
docs/                  guides; start at docs/index.md (docs/examples/* are compiled and run by ctest)
cmake/ ci/ tools/ perf/ third_party/   toolchains, CI and release tooling, perf baselines, vendored Monocypher and Level Zero header
FORGE_PLAN.md          original design and roadmap
```

## Documentation

Start at **[docs/index.md](docs/index.md)**. Contributing: [CONTRIBUTING.md](CONTRIBUTING.md). Security: [SECURITY.md](SECURITY.md).

## Licence

FORGE is dual licensed.

- **AGPL-3.0 or later** ([`LICENSE`](LICENSE)). Free for individuals, charities and not-for-profits, education and research, and organisations with annual income under AUD 50,000.
- **Commercial licence** for organisations at or above that threshold, or anyone who needs to keep modifications private. See [`COMMERCIAL-LICENCE.md`](COMMERCIAL-LICENCE.md).

Attribution line for AGPL users:

> Powered by FORGE, developed by Odin Loch. Licensed under AGPL-3.0+.

Third-party code: Monocypher (see [`NOTICE`](NOTICE)) and the MIT-licensed Level Zero API header in `third_party/level-zero`.

---

Copyright © 2026 Odin Loch, trading as Imortek. Sydney, Australia.
Enquiries: odin.loch@outlook.com.au · Licensing terms: https://imortek.com.au/licensing.html
