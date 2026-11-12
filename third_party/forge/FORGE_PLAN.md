# FORGE — Unified SIMD, GPU Compute and Inference Stack

**Architecture & Delivery Plan · v0.1**
**Author:** Odin Loch (Imortek)
**Date:** 29 October 2026
**Working name:** FORGE (rename freely — every module name below is independent of it)

---

## 0. One-page summary

One monorepo, five core layers, one design pattern running through all of them.

| Layer | Reference it replaces | What it does |
|---|---|---|
| **simd** | xsimd (+ std::simd) | SIMD on every CPU ISA; 100% raw intrinsic coverage plus a portable layer and algorithms |
| **gpu** | CUDA / HIP / SYCL | One GPU programming model over NVIDIA, AMD and Intel, plus Vulkan, Metal and a CPU backend |
| **graph** | ggml | Tensor graph compiler and executor: verify → optimise → partition → plan → replay |
| **modelfile** | GGUF / safetensors / ONNX | Model container holding the *graph*, weights, tokenizer, state schema, provenance and signature |
| **inference** | llama.cpp | Generic inference engine with **zero per-architecture code** — import a model file and run it |

Support modules: `core`, `ops`, `mem`, `task`, `io`, `prof`, `rand`, `linalg`, `vision`, `audio`, `reflect`, `serve`, `verify`.

The defining property: **code and model are fully decoupled.** llama.cpp needs new C++ for every new architecture; FORGE needs a new *file*. The engine is a compiler for model blueprints, not a collection of hand-written model builders.

---

## 1. The new pattern — "One Spec, Many Lowerings, One Oracle"

All three references solve the same problem: a matrix of **operations × targets**.

| Reference | How it solves ops × targets | Weak point |
|---|---|---|
| xsimd | Arch tags in a linear inheritance chain; kernels overloaded per arch; per-call dispatch functor | x86 features aren't linear (AVX-512 subsets, AVX10, AMX); SVE/RVV forced to fixed width; multi-arch builds risk ODR violations |
| CUDA → HIP | HIP mirrors CUDA's API surface so code ports by renaming | Inherits CUDA's stream-first, vendor-shaped model; Intel and mobile left out |
| ggml / llama.cpp | Backend vtables; per-backend kernel files; per-architecture graph builder functions in C++ | Every new model is a code change; op coverage drifts between backends |

FORGE applies five rules at every layer:

1. **Spec once.** Every operation is declared once in `ops/`: signature, shape rules, dtype rules, semantics (as a scalar reference implementation), accuracy tolerance, algebraic class (elementwise / reduction / contraction / data-movement / stateful), fusion properties and cost hints. The spec drives code generation, op registries, documentation and tests.
2. **Capabilities, not hierarchies.** Targets advertise *feature sets*. Implementations declare what they *require*. The most specific valid implementation wins — at compile time on CPU (C++20 concept subsumption), at plan time on graphs (cost-model planner).
3. **Plan once, replay forever.** Every layer separates *deciding* from *doing*. SIMD dispatch resolves once into a function table; GPU work is recorded as command graphs; tensor graphs compile into flat plans; inference sessions compile per shape bucket. Hot paths never ask "which backend am I on?".
4. **Data, not code.** Model architectures are blueprints inside the file. Quantisation formats are codecs. Importers are templates. Tokenizers are specs. Nothing about a specific model is compiled into the engine.
5. **One oracle.** The scalar reference in the spec is ground truth. Every lowering — SIMD, GPU, fused, quantised — is automatically differential-tested against it. This is where PRISM plugs in as the gate.

### 1.1 How the parts converge

```
                    ops/ (op definitions + scalar reference)
                 ┌──────────┼───────────┬─────────────┐
                 ▼          ▼           ▼             ▼
            simd codegen  gpu codegen  graph registry  verify oracle
            (CPU kernels) (GPU kernels) (op table)     (diff tests)
                 └──────────┴─────┬─────┘             │
                                  ▼                   │
                       graph plan = choose lowering ◄──┘ (only verified lowerings are eligible)
                                  ▼
                modelfile blueprint ──► inference session ──► your apps
```

Each Part below is built so it is useful on its own, then plugs into the next. The convergence milestones in Part I are the points where the parts lock together.

---

## 2. System architecture

```
 APPS     forge (CLI) · forge-serve · forge-import · forge-bench · forge-inspect
 EMBEDS   FUSE · Rallytek · Cypha / CyphaLLM · CameraDetector · NeuralVFX · PRISM
 ─────────────────────────────────────────────────────────────────────────────
 inference     sessions · state (paged KV / SSM) · samplers · batching · adapters
 modelfile     manifest · blueprint IR · tokenizer spec · tensor store · signing
 graph     IR · shape inference · fusion · partitioning · memory plan · plans
 simd     CPU SIMD (raw · portable · algorithms · tiles)
 gpu     GPU (CUDA · HIP · Level Zero/SYCL · Vulkan · Metal · CPU)
 support  mem · task · io · prof · rand · linalg · vision · audio · reflect · serve
 base     core (types, errors, mdspan, logging)  +  ops (op declarations, codegen)
 gate     verify (oracle, differential tests, fuzzing, PRISM bridge)
```

**Dependency rule — strictly downward:**
- `simd` and `gpu` know nothing about graphs.
- `graph` knows nothing about models.
- `inference` knows nothing about any specific architecture.
- Embeds (FUSE, Cypha) can take just the layers they need — `core` + `simd` is header-only and costs nothing to pull in.

---

## 3. Repository layout

```
forge/
├── CMakeLists.txt · CMakePresets.json · LICENSE · NOTICE · SECURITY.md
├── cmake/                      toolchains, target presets, sanitizers, SBOM
├── ops/
│   ├── ops/*.fspec             declarative op definitions (one file per op family)
│   ├── reference/              scalar C++ reference implementations (the oracle)
│   ├── intrinsics/             vendored machine-readable intrinsic databases
│   └── gen/                    code generators (spec → C++/kernels/docs/tests)
├── core/                       config, error, expected, dtypes (f16/bf16/fp8/int4), mdspan utils, log
├── simd/
│   ├── raw/{x86,arm,riscv,ppc,wasm,loongarch}/   GENERATED — 100% intrinsic coverage
│   ├── portable/               vec, svec, mask, ops, conversions
│   ├── math/                   vectorised libm with accuracy policies
│   ├── algo/                   transform, reduce, scan, sort, search, transpose, dot, gemm-ukernels
│   ├── tile/                   AMX / SME matrix units
│   └── dispatch/               CPU feature detection, multiversioning, dispatch tables
├── gpu/
│   ├── api/                    device, queue, buffer, view, kernel, graph, event
│   ├── kernel/                 single-source kernel dialect (item, group, subgroup, mma, stage)
│   ├── backends/{cuda,hip,level_zero,sycl,vulkan,metal,cpu}/
│   ├── prims/                  reduce, scan, sort, gemm, fft, rng, top-k, softmax, norm
│   └── loader/                 dynamic runtime loading, backend scoring, kernel cache
├── graph/
│   ├── ir/                     graph IR, symbolic shapes, dtype system
│   ├── passes/                 verify, canonicalise, fuse, layout, quant-propagate
│   ├── partition/              capability + cost planner across devices
│   ├── memplan/                liveness, arena offsets, in-place reuse
│   ├── codecs/                 quantisation codecs (pluggable)
│   └── exec/                   plan bytecode, executors, plan cache
├── modelfile/
│   ├── format/                 container reader/writer, sections, sharding
│   ├── blueprint/              blueprint language: text form, binary form, opsets
│   ├── tokenizer/              BPE, Unigram, WordPiece, byte-level — as data
│   └── trust/                  hashing, signing, encryption hooks
├── inference/
│   ├── session/                session kinds: sequence, one-shot, diffusion, streaming
│   ├── state/                  paged KV cache, prefix cache, SSM state
│   ├── sample/                 sampler chains, grammars, speculative decoding
│   ├── batch/                  continuous batching, scheduling
│   └── adapters/               LoRA hot-swap
├── import/
│   ├── templates/*.fbp         architecture templates (DATA, not code)
│   └── readers/                safetensors+config, GGUF, ONNX readers
├── support/{mem,task,io,prof,rand,linalg,vision,audio,reflect}/
├── serve/                      HTTP API, RPC device, metrics
├── bindings/{c,python}/        stable C ABI; Python via nanobind
├── apps/                       forge, forge-serve, forge-import, forge-bench, forge-inspect
├── verify/                     oracle runner, ULP harness, fuzzers, PRISM bridge, golden models
├── bench/                      micro + model benchmarks, regression baselines
└── docs/                       generated op reference, design notes, ADRs
```

---

# Part A — `simd`: SIMD on every CPU

## A.1 Four tiers

| Tier | Contents | How it's built |
|---|---|---|
| **R — raw** | Every intrinsic for every supported ISA, typed and namespaced, with its feature requirement attached as metadata | **Generated** from vendor machine-readable intrinsic databases (Intel Intrinsics Guide data, Arm ACLE intrinsic data, RISC-V RVV intrinsic spec, Power VSX, WASM SIMD). Thousands of functions — never hand-written |
| **P — portable** | `vec<T, Target>`, `svec<T>` (scalable), `mask<T, Target>`, arithmetic, compare, select, shuffle, convert, load/store (aligned, masked, gather, scatter, streaming) | Hand-written on top of Tier R |
| **A — algorithms** | transform, reduce, scan, find, compare, sorting networks, transpose, histogram, byte/string search, UTF-8 validation, dot products, GEMM micro-kernels, quantised dot kernels | Hand-written on Tier P; hot ones specialised on Tier R |
| **T — tiles** | Matrix engines: Intel AMX, Arm SME/SME2 | Separate API — tiles are not vectors |

Tier R gives "covering all intrinsics" without writing them by hand: the generator reads the database, emits `simd::raw::x86::avx512bw::mm512_maskz_add_epi8(...)`-style wrappers with strong types, `[[gnu::always_inline]]`, and a `requires` clause naming the feature. Using an intrinsic your target doesn't have becomes a **compile error with the feature name**, not a crash on a customer's machine.

## A.2 Capabilities instead of an inheritance chain

xsimd models `sse2 → sse3 → … → avx512` as a chain. Real hardware is a set: a CPU can have AVX-512 VNNI but not FP16, AVX10 without the full legacy AVX-512 naming, NEON dot-product but not i8mm. FORGE models targets as **feature sets** and picks implementations by **concept subsumption** — the most constrained valid overload wins automatically.

```cpp
namespace forge::simd {

// A target is a set of features (generated presets + user-defined)
struct x86_64_v3 : caps<sse2, sse4_2, popcnt, avx, avx2, fma, f16c, bmi2> {};
struct sapphire  : caps_extend<x86_64_v3, avx512f, avx512bw, avx512dq, avx512vl,
                               avx512vnni, avx512bf16, avx512fp16, amx_tile, amx_int8, amx_bf16> {};
struct neoverse2 : caps<neon, dotprod, i8mm, bf16, fp16, sve, sve2> {};

template <class T, class F> concept Has = T::template has<F>;

// Generic AVX2 path
template <Target T> requires Has<T, avx2>
auto popcount(vec<u8, T> v);

// More constrained → chosen automatically when available, no manual ordering
template <Target T> requires Has<T, avx2> && Has<T, avx512bitalg>
auto popcount(vec<u8, T> v);

}
```

Presets ship for the x86-64 micro-architecture levels (v2/v3/v4), named CPUs (Zen 4/5, Sapphire/Granite Rapids, Apple M-series, Neoverse V2/N2, SiFive/SpacemiT RVV parts) and a `native` preset filled from detection.

## A.3 Fixed and scalable vectors in one programming model

- `vec<T, Target>` — fixed width (SSE/AVX/AVX-512/NEON/VSX/WASM). Storable in structs.
- `svec<T>` — length-agnostic (SVE/SVE2, RVV 1.0). Register-only; width known at run time.
- `simd::strip` — strip-mining loop that hides width and tail handling. Algorithms written against the `Lane` concept work for both.

```cpp
template <simd::Target T>
void saxpy(float a, std::span<const float> x, std::span<float> y) {
    simd::strip<T, float>(x.size(), [&](std::size_t i, auto m) {   // m = tail mask
        auto vx = simd::load(x.data() + i, m);
        auto vy = simd::load(y.data() + i, m);
        simd::store(y.data() + i, simd::fma(simd::splat<T>(a), vx, vy), m);
    });
}
FORGE_SIMD_EXPORT(saxpy, targets(sapphire, x86_64_v3, neoverse2, apple_m, rvv1, scalar));
```

On AVX-512 and SVE the tail mask becomes a native predicate (no scalar epilogue). On SSE/NEON it lowers to a blend or a short masked epilogue.

## A.4 Element types

`i8…i64`, `u8…u64`, `f16`, `bf16`, `f32`, `f64`, `fp8_e4m3`, `fp8_e5m2`, packed `i4/u4`, `complex<f32/f64>`. Native where the ISA has it, emulated otherwise, with **explicit** conversion policies: rounding mode (nearest-even, toward-zero, stochastic) and saturation. No silent narrowing.

## A.5 Masks and predication

`mask<T, Target>` uses the native representation: AVX-512 `k` registers, SVE predicates, RVV `v0` masks, vector masks on SSE/NEON. Every load/store/arithmetic op has a masked form. Masks convert to/from bitfields cheaply for control flow (`any`, `all`, `none`, `first`, `count`).

## A.6 Vectorised math library

Policy-templated accuracy, so ML code and scientific code share one library:

| Policy | Target error | Use |
|---|---|---|
| `precise` | ≤ 1 ULP | Scientific, PRISM numerics, defence analytics |
| `fast` | ≤ 4 ULP | Games, DSP |
| `ml` | relative error ~1e-3…1e-4 | Activations, softmax inside inference |

Functions: `exp, exp2, expm1, log, log2, log1p, pow, sqrt, rsqrt, cbrt, hypot, sin, cos, tan, sincos, asin, acos, atan, atan2, sinh, cosh, tanh, erf, erfc, sigmoid, silu, gelu (erf & tanh forms), softplus, lgamma` plus fused reductions (`logsumexp`, `softmax` row kernels). Each function's error is **measured** by the harness, not just claimed.

## A.7 Dispatch and ODR safety

Three modes, same source:

1. **Static** — one target, chosen at compile time. Zero overhead.
2. **Multiversion** — each target compiled in its **own translation unit with its own flags**, wrapped in a per-target inline namespace. This avoids the classic ODR violation where an AVX-512-compiled inline function leaks into SSE code paths.
3. **Dispatch table** — resolved **once** at startup into function pointers (no per-call branching).

Detection covers what the OS actually enables, not just what CPUID reports: XGETBV for AVX/AVX-512 state, the Linux `arch_prctl` permission request required before AMX tiles can be used, SVE vector length via `prctl`/`rdvl`, RVV `vlenb`, `getauxval` HWCAPs on Linux/Android, `sysctlbyname` feature flags on Apple, and CPUID plus `IsProcessorFeaturePresent` on Windows. Hybrid cores (P/E cores, big.LITTLE) are reported per core type so `task` can schedule accordingly.

## A.8 Interop

- Zero-cost `.native()` to get the raw intrinsic type, and construction from it.
- Bidirectional conversion with C++26 `std::simd`. GCC 16 ships an experimental implementation that is x86-only for now, so `simd` treats it as an interop target, not a foundation.
- `std::span` / `std::mdspan` everywhere for memory.

## A.9 ISA coverage matrix

Phase numbers refer to the roadmap in Part I.

| Family | Extensions | Phase |
|---|---|---|
| Any | scalar reference (always) | 0 |
| x86-64 | SSE2–SSE4.2, AVX, AVX2, FMA3, F16C, BMI1/2 | 1 |
| x86-64 | AVX-512 (F/CD/BW/DQ/VL/VNNI/BF16/FP16/IFMA/VBMI/VBMI2/BITALG/VPOPCNTDQ), AVX-VNNI | 1 |
| x86-64 | AVX10 | 5 |
| x86-64 | AMX (tile, int8, bf16, fp16) | 7 |
| AArch64 | NEON/ASIMD, dotprod, i8mm, bf16, fp16 | 1 |
| AArch64 | SVE, SVE2 (length-agnostic) | 5 |
| AArch64 | SME, SME2 (tiles) | 7 |
| RISC-V | RVV 1.0, Zvfh, Zvbb | 5 |
| Power | VSX (Power9/10) | post-v1 |
| WebAssembly | SIMD128, relaxed SIMD | post-v1 |
| LoongArch | LSX, LASX | post-v1 |

## A.10 Testing `simd`

- **Exhaustive** testing for every 8- and 16-bit type op (all inputs, all lanes).
- Edge-case corpora for 32/64-bit: ±0, ±inf, NaN payloads, denormals, boundaries, overflow points.
- **ULP harness** for every math function across full ranges with sampling + known hard cases.
- **Emulated CI**: Intel SDE for AVX-512/AMX/AVX10 on machines without them; QEMU with SVE vector lengths from 128 to 2048 bits and multiple RVV VLENs. A bug that only appears at SVE VL=384 gets caught before a customer finds it.

---

# Part B — `gpu`: one GPU programming model

## B.1 The model (not a CUDA clone)

CUDA's model is stream-first: you launch things into streams and optionally capture graphs. FORGE is **graph-first**: everything submitted is a node in a command graph; "eager" mode is just a one-node graph that runs immediately. This maps natively onto CUDA graphs, HIP graphs, SYCL graphs, Vulkan command buffers and Metal indirect command buffers — all of which now exist.

| Concept | Meaning |
|---|---|
| `Device` | One GPU (or CPU) with a normalised capability report |
| `Queue` | Ordered submission lane on a device |
| `Buffer<T>` / `Image` | Owned memory with explicit residency |
| `View` | `std::mdspan`-based typed view with layout (row/col/blocked/strided) |
| `Kernel` | A C++ functor written once in the kernel dialect |
| `Graph` → `Executable` | Recorded work → compiled, replayable form |
| `Event` / `Fence` | Cross-queue and cross-device sync |

```cpp
struct Scale {
    float a;
    FORGE_KERNEL void operator()(gpu::item<1> it, gpu::span<float> y) const {
        if (auto i = it.global(0); i < y.size()) y[i] *= a;
    }
};

auto dev = gpu::select(gpu::prefer::discrete);   // NVIDIA, AMD, Intel, Apple… or CPU
auto q   = dev.queue();
auto buf = dev.alloc<float>(n);

gpu::graph g;
auto up = g.copy(host_span, buf);
auto k  = g.launch(Scale{2.0f}, gpu::extent{n}, buf).after(up);
g.copy(buf, host_span).after(k);

auto exe = g.compile(q);   // → CUDA graph / HIP graph / SYCL graph / Vulkan cmdbuf / Metal ICB
exe.run();                 // replay with near-zero launch overhead
exe.run();
```

## B.2 Normalised device vocabulary

| Vendor term (NVIDIA / AMD / Intel) | FORGE term | Notes |
|---|---|---|
| warp / wavefront / sub-group | `subgroup` | Width 32 / 32 or 64 / 8–32 — a compile-time property of the kernel target |
| shared memory / LDS / SLM | `group_memory` | Static and dynamic sizes |
| SM / CU / Xe-core | `compute_unit` | |
| Tensor Core / MFMA·WMMA / XMX | `matrix_unit` | Exposed via `gpu::mma` |
| CUDA graph / HIP graph / SYCL graph | `graph` | Conditional nodes used where available |
| TMA / async LDS copies / 2D block loads | `stage` | Async copy pipeline abstraction |

## B.3 Kernel dialect

- `item<N>`: global / local / group IDs, extents.
- `group_memory<T, N>` and dynamic group memory; `group_barrier()`.
- Atomics with **explicit scopes**: subgroup, group, device, system (`std::atomic_ref` semantics).
- Subgroup ops: shuffle, broadcast, ballot, reduce, inclusive/exclusive scan, vote.
- `gpu::mma<M, N, K, A, B, Acc>` — maps to NVIDIA tensor-core instructions, AMD MFMA/WMMA, Intel joint-matrix/DPAS; falls back to subgroup FMA loops.
- `gpu::stage<Depth>` — multi-buffered async global→group-memory pipelines.
- Restrictions enforced at compile time: no exceptions, no RTTI, no virtuals, no host-only std calls.

## B.4 Backends and how kernels reach them

| Backend | Toolchain | Kernel path | Host runtime | Phase |
|---|---|---|---|---|
| **CPU** | host compiler | loop nest over `simd` + `task` | in-process | 2 |
| **NVIDIA** | clang-CUDA or nvcc (CUDA 13.x, Turing → Blackwell) | `__global__` trampoline → PTX + cubin fatbin | CUDA Driver API, dynamically loaded | 2 |
| **AMD** | clang/hipcc (ROCm 7.x) | `__global__` trampoline → AMDGPU code objects per gfx target | HIP runtime, dynamically loaded | 2 |
| **Intel** | icpx / intel-llvm SYCL | `parallel_for` wrapper → SPIR-V or AOT binaries | Level Zero directly (SYCL runtime optional) | 5 |
| **Vulkan** | generated SPIR-V (or Slang source) | compute shaders | Vulkan 1.3 compute | 6 |
| **Metal** | generated MSL (or Slang source) | compute kernels | Metal | 6 |

**Honest constraint:** C++ functors compile directly for CPU, CUDA, HIP and SYCL. They do **not** compile to Vulkan or Metal. For those two, kernels come from the `ops` code generator (or from Slang, a multi-target shading language). This is tractable because the kernel set an inference engine and game engine actually need is finite — on the order of a couple of hundred kernels — and they are exactly the ones `ops` already describes.

CUDA and HIP share ~90% of their backend code because their models are near-identical. That's why both land in the same phase.

## B.5 Memory

- Residency kinds: `device`, `host_pinned`, `host_mapped`, `managed` (unified), `peer`.
- Stream-ordered pool allocator per device; sub-allocation for small tensors.
- Pinned staging rings for uploads; zero-copy paths on unified-memory parts (Apple, APUs, Grace-class systems, Jetson).
- Graphics interop: share buffers between FUSE's renderer (Vulkan/Metal) and compute (CUDA/HIP) via external memory/semaphores — needed for in-game inference without copies.

## B.6 `gpu::prims` — portable primitives

Reduce, scan, stream compaction, radix sort (keys and pairs), segmented ops, histogram, GPU hash map, GEMM / batched GEMM / GEMV (with epilogue fusion), convolution, FFT, Philox RNG, top-k, softmax, layer/RMS norm, flash-style attention.

**Vendor escape hatch:** if cuBLAS / hipBLASLt / oneMKL are present, the planner may use them — but only if they beat FORGE's own kernel in the on-device benchmark **and** pass the oracle. FORGE's kernels are the portable baseline; vendor libraries are optional accelerators, never dependencies.

## B.7 Loading, packaging, caching

- Vendor runtimes are `dlopen`ed — **one binary runs on any machine**; missing vendors are simply absent backends.
- Backends are plugins that return a capability report; the loader scores them (like ggml's best-backend scoring, but on capabilities and measured throughput, not a single number).
- Fat binaries carry AOT kernels for listed targets; an on-disk kernel cache is keyed by device + driver + kernel hash.
- Optional runtime compilation (NVRTC / hipRTC / SYCL runtime compile) for shape-specialised kernels.

## B.8 Tooling hooks

`prof` markers surface in Nsight Systems, rocprof and VTune; CI runs kernels under compute-sanitizer (NVIDIA) and address sanitizer on the CPU backend. Because every kernel also runs on the CPU backend, **most GPU bugs can be reproduced and debugged on a laptop with no GPU.**

---

# Part C — `graph`: tensor graph compiler and executor

ggml builds a graph of tensor structs and walks it each time. `graph` treats the graph as a **program to compile once** and an execution plan to **replay**.

## C.1 Pipeline

```
build ─► verify ─► canonicalise ─► optimise ─► partition ─► memory-plan ─► lower ─► plan ─► replay
          shapes      CSE, const      fusion,     devices by    liveness,     pick      flat     N times
          dtypes      folding         layout,     capability    arena         verified  command
          bounds                      quant prop  + cost        offsets       kernels   list
```

| Stage | What it does |
|---|---|
| **verify** | Shape and dtype inference from spec rules; rejects invalid graphs with exact op/tensor locations |
| **canonicalise** | Common-subexpression elimination, constant folding, dead-code removal, algebraic simplification |
| **optimise** | Fusion (elementwise chains, GEMM + bias + activation epilogues, norm + quantise, attention), layout selection (row/col/blocked/tiled per device), quantisation propagation |
| **partition** | Assigns subgraphs to devices using capability reports and a cost model (FLOPs, bytes moved, transfer cost); supports weight offload and layer splitting across GPUs |
| **memory-plan** | Liveness analysis → static arena offsets; in-place reuse; peak-memory minimisation; reports exact memory need **before** running |
| **lower** | Picks the most specific *verified* kernel for each op on its device |
| **plan** | Emits a flat command list (bytecode for CPU; `gpu::graph` for GPU) |
| **replay** | Executes the plan; only pointers and scalar parameters change between runs |

## C.2 Symbolic shapes and plan caching

Dimensions can be symbols (`batch`, `seq`, `ctx`). Plans are compiled with symbolic offsets and cached per **shape bucket** (e.g. sequence lengths rounded to powers of two for prefill, fixed shape for decode). Dynamic sequence lengths don't trigger recompilation on every token.

## C.3 Quantisation as pluggable codecs

Quantisation formats are **codecs**, not hard-coded dtypes. A codec provides: block layout, pack, unpack, and per-backend dot / GEMM micro-kernels, each verified against the dequantise-then-compute reference.

Planned codecs:
- Super-block k-quant style (2–8 bit) for GGUF compatibility
- Group-wise weight-only int4/int8 (AWQ/GPTQ-style scales and zero points)
- FP8 (E4M3/E5M2) with per-tensor or per-block scales
- OCP Microscaling (MXFP8/MXFP6/MXFP4/MXINT8) and NVFP4, native on the newest NVIDIA/AMD datacentre parts, emulated elsewhere
- Ternary/binary (BitNet-style) for very small edge models

New codecs drop in without touching the engine. Mixed precision per layer is a plan decision, not a file format limitation.

## C.4 Extras in `graph`

- **Determinism mode**: fixed reduction orders and deterministic atomics → bit-identical results across runs on the same device. Needed for game replays, audit trails and debugging.
- **Reverse-mode autodiff** (later phase): enables LoRA fine-tuning and on-device adaptation with the same IR.
- **Graph capture from C++**: build graphs with a fluent API for non-model compute (PRISM solvers, physics, image pipelines).

---

# Part D — `modelfile`: the model container (`.fpk`)

This is what makes "import model data and run it" possible. The file is self-describing: it contains **what to compute**, not just numbers.

## D.1 Compared with existing formats

| Format | Weights | Metadata | Graph | Tokenizer | State schema | Integrity / signing |
|---|---|---|---|---|---|---|
| safetensors | ✔ | JSON header | ✘ | ✘ | ✘ | ✘ |
| GGUF | ✔ | key/value | ✘ — architecture string selects C++ code | ✔ | ✘ | ✘ |
| ONNX | ✔ | ✔ | ✔ | ✘ | partial | ✘ |
| **.fpk** | ✔ | typed manifest | ✔ blueprint | ✔ as data | ✔ | ✔ content hashes + signatures |

## D.2 Layout

```
┌──────────────────────────────────────────────┐
│ header: magic, version, endianness, alignment │  fixed size
├──────────────────────────────────────────────┤
│ section table: kind, offset, size, hash       │
├──────────────────────────────────────────────┤
│ MANIFEST    typed metadata, opset, provenance │
│ BLUEPRINT   graph program (binary form)        │
│ TOKENIZER   tokenizer / preprocessor spec      │
│ STATE       state schema (KV, SSM, buffers)    │
│ TENSORS     index: name, dtype, codec, shape,  │
│             offset, BLAKE3 hash                │
│ [ADAPTERS]  optional LoRA sets                 │
│ [CALIB]     optional calibration / eval data   │
│ [LICENCE]   licence text + usage terms         │
│ [SIGN]      signatures over manifest + hashes  │
├──────────────────────────────────────────────┤
│ tensor blobs — each aligned (4 KiB default,    │
│ 64 KiB option) for direct mmap and DMA upload  │
└──────────────────────────────────────────────┘
```

## D.3 Properties

- **mmap-first**: tensors are page-aligned; loading a model is mapping it. Weights can be paged from disk on demand (large models on small RAM).
- **Content-addressed tensors**: every blob has a BLAKE3 hash. Fine-tunes that share a base model store only the changed tensors; shared tensors are deduplicated across files; integrity is verifiable per tensor.
- **Sharding**: one manifest, many blob files; partial downloads; streaming load.
- **Signing**: Ed25519 signatures over the manifest and hash table — supply-chain integrity a defence customer can verify. Use a vetted library (libsodium or Monocypher); **do not write your own crypto**.
- **Encryption hook**: optional AES-GCM encryption of tensor blobs with externally supplied keys, for delivering model IP to customers.
- **Opsets**: blueprints declare an opset version (like ONNX). The engine supports a range; old files keep working; new ops require a newer engine, and the error message says exactly which op and which opset.
- **Two forms**: binary for shipping, text (`.fbp`) for authoring, diffing and review. Lossless round-trip.

---

# Part E — `inference`: the generic inference engine

## E.1 What "decoupled" means concretely

```
llama.cpp:   model.gguf ──► "arch = llama" ──► llm_build_llama()  (C++ per architecture)
FORGE:       model.fpk  ──► blueprint      ──► graph compiler      (no model-specific code)
```

Adding a new architecture = writing (or importing) a blueprint. No engine rebuild, no new release.

## E.2 The blueprint language

A small, declarative, statically-typed graph language with symbolic dimensions, named parameters, typed state, and structured repetition.

```
blueprint "decoder-lm" opset 1

params {
  d_model: 4096   n_layers: 32   n_heads: 32   n_kv: 8   head_dim: 128
  vocab: 128256   eps: 1e-5
  rope: { base: 500000, scaling: yarn(factor: 8, orig_ctx: 8192) }
}

state kv[n_layers] : paged_kv(heads: n_kv, dim: head_dim, dtype: f16)

input  tokens    : i32[batch, seq]
input  positions : i32[batch, seq]

graph {
  x = embed(W.tok_emb, tokens)
  repeat l in 0..n_layers {
    h       = rms_norm(x, W.layer[l].attn_norm, eps)
    q, k, v = split(matmul(h, W.layer[l].wqkv), [n_heads, n_kv, n_kv] * head_dim)
    q, k    = rope(q, k, positions, rope)
    a       = attention(q, kv[l].append(k, v), causal: true, groups: n_heads / n_kv)
    x       = x + matmul(a, W.layer[l].wo)
    h       = rms_norm(x, W.layer[l].ffn_norm, eps)
    x       = x + matmul(silu(matmul(h, W.layer[l].w_gate)) * matmul(h, W.layer[l].w_up),
                         W.layer[l].w_down)
  }
  logits = matmul(rms_norm(x, W.out_norm, eps), W.lm_head)
}

output logits : f32[batch, seq, vocab]
session generative_sequence
```

What the language must express to cover real models (and therefore what Phase 4–7 build):

| Feature | Covers |
|---|---|
| Attention variants as one op with attributes | MHA, GQA, MQA, MLA (latent attention), sliding window, attention sinks, cross-attention |
| Positional schemes as attributes | RoPE (NTK, YaRN, LongRoPE-style), ALiBi, learned |
| Typed state slots | Paged KV cache, SSM/recurrent state (Mamba/RWKV-style), conv state |
| Routing ops | Mixture-of-experts top-k gating, expert gather/scatter, shared experts |
| Structured repetition and `scan` | Layers, recurrent cells, diffusion step loops |
| Sub-graphs and multiple entry points | Encoder/decoder, vision tower + projector, prefill vs decode |
| Composite ops defined in-file | A new op written as a composition of existing primitives, so new architectures run on older engines until a fused kernel exists |

## E.3 Session kinds — runtime services chosen by the file

The blueprint declares its session kind; the engine attaches the generic services.

| Session kind | Services attached | Your use |
|---|---|---|
| `generative_sequence` | tokenizer, paged KV, sampler chain, batching, speculative decoding | LLMs |
| `one_shot` | preprocessor, batching | Classifiers, embeddings, detectors, seatbelt/phone detection |
| `diffusion_loop` | noise scheduler, guidance, step plan, latent cache | CameraDetector (macro/micro stages), Rallytek world builder |
| `streaming` | ring buffers, frame scheduler, latency budget | NeuralVFX, live audio/video |
| `custom` | user-registered service plugin | Cypha models (see Part F) |

## E.4 Engine features

**State & memory**
- Paged KV cache with block tables; prefix caching (shared system prompts computed once); KV quantisation (fp8/int8/int4); KV offload to host.

**Throughput**
- Continuous batching with chunked prefill; priority scheduling; per-request cancellation.
- Speculative decoding: draft model, n-gram/lookahead, self-speculation (early-exit) — all as blueprint options.

**Output control**
- Sampler chains: temperature, top-k, top-p, min-p, typical, repetition/presence penalties, Mirostat-style — composable and declared per request.
- Constrained decoding: GBNF-style grammars and JSON Schema compiled into token masks.

**Adaptation**
- LoRA/adapter hot-swap per request without reloading base weights.

**Scale**
- Multi-GPU: layer (pipeline) split and tensor split, mixed vendors allowed (e.g. an NVIDIA and an AMD card in one box).
- CPU + GPU hybrid with weights mmap'd and streamed.
- RPC device: run part of a model on another machine through `serve`.

**Modalities**
- Text, vision encoders, audio encoders/decoders, diffusion — all through the same blueprint + session mechanism.

## E.5 Importers — the only place that knows model families

```
forge import ./hf-model-folder        →  model.fpk   (safetensors + config.json)
forge import model.gguf               →  model.fpk
forge import model.onnx               →  model.fpk
forge run    model.fpk  "prompt"
forge run    ./hf-model-folder        (on-the-fly import, no conversion step)
```

Importers read the source config, pick an **architecture template** from `import/templates/` (itself a parameterised blueprint — data, not code), bind parameters and weight names, optionally quantise, and write a `.fpk`. Supporting a new model family on day one = writing a template file.

Initial templates: Llama-family decoders, Qwen, Mistral/Mixtral (MoE), Gemma, Phi, DeepSeek-style MoE with MLA, Mamba/hybrid SSM, BERT-style encoders, ViT/SigLIP-style vision towers, Whisper-style speech, UNet and DiT diffusion.

## E.6 APIs

```cpp
auto model = forge::inference::open("model.fpk");                 // verify signature, map tensors
auto sess  = model.session({ .devices = forge::gpu::all(),
                             .context = 32'768,
                             .determinism = false });
for (auto t : sess.generate("Describe the scene.", { .temperature = 0.7f }))
    std::cout << t.text;
```

- **Stable C ABI** (`forge.h`) — embed in FUSE, Cypha, anything; ABI-versioned.
- **C++ API** — the one above.
- **Python** via nanobind — for experiments and tooling.
- **`forge-serve`** — HTTP server with an OpenAI-compatible chat/completions/embeddings surface, plus metrics; offline by default.

---

# Part F — Extras for your application code

## F.1 Support modules (shared by everything)

| Module | Contents | Why it matters |
|---|---|---|
| `task` | Work-stealing pool, task graphs, priorities, P/E-core and big.LITTLE aware placement, frame-paced mode, `parallel_for` with SIMD-aware chunking | One scheduler for FUSE frames, CPU inference and PRISM jobs — no thread oversubscription |
| `mem` | Arenas, pools, frame allocators, TLSF, huge pages, NUMA placement, GPU sub-allocator, allocation tracking and leak reports | Games and inference both live or die on allocation behaviour |
| `io` | mmap, async file I/O (io_uring / IOCP / dispatch_io), asset streaming, later direct-to-GPU storage | Model loading and FUSE asset streaming share one path |
| `prof` | Scoped zones, counters, GPU timestamps, Perfetto / Chrome-trace export, optional Tracy | One timeline for CPU, GPU and inference |
| `rand` | Counter-based RNG (Philox, Threefry) with **identical streams on CPU and every GPU** | Reproducible diffusion, deterministic game replays |
| `linalg` | GEMM on `simd` micro-kernels + tile units; LU/QR/Cholesky/SVD/eig for small and medium sizes; sparse CSR/BSR SpMV/SpMM; CG/GMRES | Physics, vision geometry, PRISM numerics, analytics |
| `vision` | Image decode (via vetted libs), resize, colour conversion, normalisation, NMS, ROI-align, video-decode hooks (NVDEC, VA-API, VideoToolbox, MediaCodec), camera capture | CameraDetector, seatbelt/phone detection, Rallytek street imagery |
| `audio` | FFT, STFT, mel filterbanks, resampling, filters, ring buffers | NeuralVFX, FUSE audio, speech models |
| `reflect` | Serialisation with C++26 reflection behind a feature macro where the compiler supports it, macro fallback otherwise | Save games, config, network messages, editor property panels |

## F.2 Project-by-project integration

**FUSE (engine + GUI framework)**
- `task`, `mem`, `io`, `prof` become FUSE's runtime services — one implementation, not two.
- `gpu` Vulkan/Metal backends are FUSE's compute layer: GPU particles, culling, skinning, physics broad-phase, all written once and running on Android, iOS, Linux, macOS and Windows.
- `simd` accelerates the GUI framework's CPU paths: blending, colour conversion, glyph rasterisation, layout math.
- `inference` embeds through the C ABI for in-engine AI tools (editor assistants, asset tagging) and in-game models.

**Rallytek**
- Cypha racing AI runs per frame on CPU via `simd` + `task`, inside the frame budget, with determinism mode so replays and ghost cars are bit-identical.
- The real-world track builder runs as a `diffusion_loop` / vision pipeline over street-level imagery in the editor or offline via `forge-serve`, outputting into FUSE's asset pipeline.

**Cypha / CyphaLLM**
- Cypha takes **only `core` + `simd`** (header-only): no extra weight, no new code debt, and the single-core classifier gets explicit SIMD on every target.
- Cypha's data packs (dictionary, thesaurus, text statistics) can ship as `.fpk` sections — signed, versioned, mmap'd — so code and data stay decoupled for Cypha too.
- Optional: a `custom` session plugin lets apps call Cypha through the same `inference` API as neural models, without Cypha itself depending on `inference`.

**CameraDetector**
- Macro and micro stages become two sub-graphs in one blueprint under `diffusion_loop`; `vision` handles pre/post-processing; `gpu` runs it on whatever GPU the customer has.

**Seatbelt / phone detection**
- `one_shot` session with a fixed latency budget; same `.fpk` deploys to x86 boxes, Jetson-class boards (CUDA on Arm) and Android devices (Vulkan).

**NeuralVFX**
- `streaming` session + `audio` module for low-latency processing; Philox RNG for reproducible generation.

**PRISM**
- `gpu::prims` (radix sort, scan, compaction, GPU hash tables, segmented reductions) are exactly the building blocks of GPU-side solver work such as clause preprocessing and parallel local search — the CUDA solver goal gets AMD and Intel support for free.
- PRISM is FORGE's verification gate (Part G) and FORGE is a large real-world C++23 codebase for PRISM to prove itself on.

## F.3 Defence-grade properties (selling points)

- **Offline by default**: no network, no telemetry; `serve` must be explicitly enabled.
- **Signed models and releases**: verify before load; refuse unsigned files in strict mode.
- **Deterministic mode**: reproducible outputs for audit and incident review.
- **Air-gap friendly**: single binary, dynamic vendor loading, no package-manager calls at runtime.
- **SBOM + reproducible builds**: SPDX/CycloneDX generated in CI.
- **Permissive-only dependencies**: MIT/BSD/Apache/Zlib — nothing that complicates a government sale.

---

# Part G — Verification and hardening

| Mechanism | Scope |
|---|---|
| **Oracle matrix** | Every (op × backend × dtype × shape class) cell runs against the scalar reference; results published as a coverage table, so op-coverage drift between backends is visible, not discovered by users |
| **Tolerance policies** | Per op and dtype (exact for integer ops, ULP bounds for math, relative/absolute bounds for reductions and low precision) |
| **Golden models** | Small reference models with recorded logits; greedy decode must match token-for-token within logit tolerance on every backend |
| **Pass equivalence** | Random graphs: optimised plan must equal unoptimised plan within tolerance |
| **Fuzzing** | Container parser, blueprint parser, tokenizer, grammar compiler, HTTP server — libFuzzer + ASan/UBSan. Model files are untrusted input; the parser is attack surface |
| **Formal targets (PRISM)** | Memory planner never overlaps live buffers and keeps every access in bounds; strip-mine tail handling is in-bounds for all lengths; codec pack/unpack round-trips; container section bounds checks |
| **Performance regression (PRISM)** | Per-device benchmark baselines; CI fails on regressions beyond a set threshold |
| **Sanitizers** | ASan/UBSan/TSan on CPU paths; compute-sanitizer on NVIDIA |

---

# Part H — Build, toolchains, CI

**Language**: C++23 baseline (matches PRISM). C++26 features — `std::simd` interop, reflection — behind feature macros.

**Build**: CMake with presets; each module its own target; `core` and `simd` are header-only interface targets. Device-facing code stays in headers because GPU compilers' support for C++20 modules is limited; host-only libraries can adopt modules later.

**Compilers**: GCC 15/16, Clang 20+, MSVC (Visual Studio 2026), Apple Clang, nvcc / clang-CUDA (CUDA 13.x), ROCm clang/hipcc (7.x), Intel icpx / intel-llvm (2026.x).

**Platforms**: Linux, Windows, macOS, Android, iOS — the same set FUSE targets.

**CI matrix**

| Runner | Covers |
|---|---|
| Linux x86-64 + Intel SDE | SSE → AVX-512, AVX10, AMX in emulation |
| Linux AArch64 + QEMU | NEON, SVE/SVE2 at multiple vector lengths |
| QEMU RISC-V | RVV 1.0 at multiple VLENs |
| Windows MSVC | ABI, dispatch, Windows I/O |
| macOS Apple Silicon | NEON, Metal |
| Android NDK / iOS cross | Mobile builds, Vulkan/Metal |
| Self-hosted GPU box(es) | One NVIDIA, one ROCm-supported AMD card, one Intel Arc — the minimum lab for honest cross-vendor claims |

**Dependency policy**: minimal, vendored with licences in `NOTICE`, no Boost, no runtime downloads.

---

# Part I — Roadmap and convergence milestones

Sizes are relative (S < M < L < XL). The **critical path** is Phases 0 → 1 (x86 + NEON only) → 2 (CPU + CUDA + HIP) → 3 → 4. Everything after Convergence 1 is breadth.

| Phase | Size | Deliverables | Exit criteria |
|---|---|---|---|
| **0 — Foundations** | M | `core`, spec format and generator skeleton, ~60 reference ops, `verify` harness, CMake, emulated CI | One op written in `ops` automatically yields its reference implementation, tests and docs |
| **1 — simd core** | L | Tier R generator (x86, Arm); Tier P on SSE→AVX-512, NEON, scalar; math lib (`fast`, `ml`); dispatch; core algorithms; f32/bf16/int8 GEMM micro-kernels | All Tier P ops pass the oracle on x86, NEON and scalar; GEMM hits a stated percentage of a vendor BLAS on a reference machine |
| **2 — gpu core** | L | API, graph-first execution, CPU + CUDA + HIP backends, core prims, loader, kernel cache | Same kernel source runs on CPU, NVIDIA and AMD; prims pass the oracle on all three |
| **3 — graph** | L | IR, verify, passes, partitioner, memory planner, plans; first codecs (f16, bf16, int8, 4-bit k-quant) | Graph compiled once and replayed; predicted peak memory equals measured |
| **4 — modelfile + inference MVP** | XL | `.fpk` reader/writer, blueprint text + binary, BPE tokenizer, safetensors/HF and GGUF importers, `generative_sequence`, KV cache, sampler chain, CLI | **Convergence 1 — "One file, three targets":** a Llama-class model imported from HF runs from one `.fpk` on CPU, NVIDIA and AMD with zero model-specific code, matching the golden reference |
| **5 — Breadth** | XL | Intel (Level Zero / SYCL); SVE/SVE2/RVV; paged KV + prefix cache; continuous batching; speculative decoding; grammars; MoE + MLA; FP8 and MX codecs; multi-GPU; `forge-serve`; C ABI + Python | **Convergence 2 — "Every desktop GPU":** same file on NVIDIA, AMD and Intel; server handles concurrent requests |
| **6 — Mobile + engine** | L | Vulkan + Metal backends from spec codegen; Android/iOS builds; graphics interop; FUSE runtime services moved onto `task`/`mem`/`io`/`prof` | **Convergence 3 — "In the game":** FUSE runs Cypha AI and a small model on Android and iOS through the same stack |
| **7 — Modalities + your apps** | L | `diffusion_loop`, `streaming`, `vision`, `audio`; CameraDetector and seatbelt detection on edge; PRISM GPU solver prims; AMX/SME tiles; LoRA hot-swap; optional autodiff | **Convergence 4 — "Whole portfolio, one stack"** |
| **8 — Hardening + v1.0** | M | Signing, encryption hook, SBOM, reproducible builds, fuzzing campaigns, generated docs, performance baselines | Public v1.0 with signed release artefacts |

Parallelism: Phase 2's CPU backend can start alongside Phase 1, and the `.fpk` reader (Phase 4) can start as soon as Phase 0's `core` exists.

---

# Part J — Risks and honest constraints

- **Scale.** xsimd, HIP/SYCL, ggml and llama.cpp each represent years of work by large contributor bases. Solo, the only way through is ruthless sequencing: reach Convergence 1 before any breadth. AI coding agents are well suited to the generated and repetitive parts (Tier R wrappers, backend boilerplate, importer templates) — and the oracle is what makes that safe, because nothing ships that hasn't matched the reference.
- **Peak performance.** Matching tuned vendor libraries on GEMM and attention is hard. The vendor escape hatch (B.6) means FORGE is never slower than the vendor where the vendor library exists, while staying portable where it doesn't.
- **Vendor churn.** New GPU generations arrive yearly and support lists change. Keep backends thin; put intelligence in `graph` and `ops`.
- **Blueprint expressiveness.** Some new architectures will need ops the engine lacks. Mitigation: **composite ops** — a file can define a new op as a composition of existing primitives, so new models run (slower) on older engines, and a fused kernel can be registered later without changing the file.
- **Licensing and provenance.** xsimd (BSD-3) and llama.cpp (MIT) permit reuse with notices retained, but a clean-room implementation of a new pattern gives clean provenance for government customers; record anything derived in `NOTICE`. CUDA's headers and libraries are under NVIDIA's licence — mirror concepts, never code. Model weights carry their own licences; `modelfile` records them and the importer never strips them.
- **Project licence.** FORGE is dual licensed: AGPL-3.0-or-later plus a commercial licence (see `LICENSE` and `COMMERCIAL-LICENCE.md`). Dual licensing only works while all copyright is held by the author, so any outside contributions need a contributor licence agreement before they are merged.
- **Export obligations.** Shipping encryption and high-performance AI software to overseas government customers can bring the cryptography and dual-use categories of the Defence and Strategic Goods List into play. Check obligations with Defence Export Controls before exporting builds with the encryption hook enabled.

---

# Appendix — First 20 concrete steps

1. Create the repo with the layout in §3, CMake presets and an empty CI matrix.
2. Write `core/dtypes`: f16, bf16, fp8 (E4M3/E5M2), packed int4 — scalar, exhaustively tested.
3. Define the `.fspec` format (YAML-like or a small custom grammar) and write the generator skeleton.
4. Write specs + scalar references for the first 20 ops: add, mul, fma, select, convert, reduce-sum/max, exp, log, tanh, sigmoid, silu, softmax, rms_norm, matmul, transpose, gather, rope, attention (naive).
5. Build the `verify` oracle runner and tolerance policies; wire it into CI.
6. Vendor the intrinsic databases into `ops/intrinsics/` and generate Tier R for x86.
7. Implement `caps`, target presets and concept-subsumption selection; test with a two-overload example.
8. Implement `vec`, `mask`, load/store (incl. masked) on AVX2 and AVX-512.
9. Implement `strip` and port `saxpy`, reductions and `softmax` row kernel.
10. Implement CPU feature detection (incl. XGETBV and AMX permission) and the dispatch table.
11. Add NEON to Tier R and Tier P; run under QEMU in CI.
12. Write the vectorised `exp/log/tanh/sigmoid/silu` with `fast` and `ml` policies and the ULP harness.
13. Build an f32 GEMM micro-kernel on AVX-512 and NEON; benchmark against a vendor BLAS.
14. Define `gpu` API headers (device, queue, buffer, view, graph) and the CPU backend.
15. Implement the CUDA backend: dynamic driver loading, fatbin loading, graph compile/replay.
16. Port the backend to HIP; run the same kernels on an AMD card.
17. Implement `gpu::prims` reduce, scan and GEMM; pass the oracle on CPU, NVIDIA, AMD.
18. Start `graph` IR with shape inference and the memory planner.
19. Write the `.fpk` writer/reader and the blueprint text parser; round-trip test.
20. Write the Llama-family import template and run a small model end-to-end on CPU — the first step of Convergence 1.
