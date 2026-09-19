# RetDec — Future Directions

**v2.0.24 (2026-09).** This file is a roadmap, not a feature list.
Shipped product claims live in [README.md](../README.md) and
[docs/CLAIMS.md](CLAIMS.md). Dual licence: AGPL-3.0+ or commercial.

## How to read this

| Kind | Meaning at v2.0.24 |
|------|---------------------|
| **Shipped** | Linux + macOS installers, Windows installer via GitHub Actions (`release-installers.yml`), Qt 6 GUI (`RetDec.app` on macOS), specification-extraction positioning, **buildable C default**, input-keyed outputs, optional llama.cpp (`RETDEC_ENABLE_LLAMACPP` / `RETDEC_NEURAL_REFINE`). |
| **Measured, not product-quality** | Structural detectors exist (`src/container_detect`, `src/algo_recover`, `src/sort_detect`, …). Name-blind algorithm-recovery F1 on the 216-binary stand-in is **0.056**. Do not advertise 1.0. |
| **Open research** | Anything not wired into `src/retdec`, rejected at the CLI, or parked. CUDA/OpenCL pipeline acceleration (`C-CUDA-PIPE`) is **withdrawn**. `--output-lang cpp` is **rejected**. |

The three parts below are:

1. **Semantic library recovery** — detectors that already run vs reconstruction quality that is still open.
2. **New decompilation targets** — architectures, formats, and output languages.
3. **Research frontiers** — speculative work. Detail: [RESEARCH_FRONTIERS.md](RESEARCH_FRONTIERS.md).

---

## Part 1 — Semantic Library Recovery

> Recovering standard-library usage is the highest-leverage *research*
> improvement available: STL appears in nearly every non-trivial C++ binary.
> **v2.0.24 already ships structural detectors** as part of specification
> extraction. What is *not* shipped is product-quality, name-blind recovery
> or guaranteed idiomatic `v.push_back()` emission.

### The Core Problem

Signature-based matching (FLIRT-style) works for non-templated library
functions with stable binary representations. It **fails** for STL because
templates are instantiated per type — `std::sort<int*>` and
`std::sort<std::string*>` compile to different machine-code sequences with
no shared binary signature.

The research solution is **semantic matching**: abstract characterisations of
*what* an algorithm does rather than what its bytes look like. The current
detectors are a first cut of that idea; name-blind F1 0.056 is the honest
score.

---

### Algorithm Recovery

Detectors live under `src/sort_detect` and `src/algo_recover`. Labels are
exported on `.config.json` as `semanticDetections`. Harness:
[tests/algorithm_recovery/README.md](../tests/algorithm_recovery/README.md).

#### Sorting

| Algorithm | Structural invariants | Status |
|-----------|----------------------|--------|
| Quicksort / Introsort | Two-index partition; two recursive calls; heapsort fallback; insertion-sort switch-over | Detector in-tree; quality open |
| Merge sort | Two recursive half-calls; merge loop; auxiliary buffer | Detector in-tree; quality open |
| Heapsort | Heap-build + sift-down; child index `2i+1`, `2i+2` | Detector in-tree; quality open |
| Radix sort | Histogram / prefix-sum / scatter; no comparisons | Detector in-tree; quality open |

**Detection strategy (research):** characterise functions by loop nesting,
recursive call count, comparison-swap pairing, auxiliary memory, and index
arithmetic. Match against descriptors with confidence thresholds.

#### Searching

| Algorithm | Key signal | Status |
|-----------|-----------|--------|
| Binary search | `(low + high) >> 1`; three-way branch | Detector in-tree; quality open |
| Linear search (`std::find`) | Range loop with equality and early exit | Detector in-tree; quality open |
| Interpolation search | Non-uniform midpoint | Research |

#### Graph Algorithms

| Algorithm | Key signal | Status |
|-----------|-----------|--------|
| BFS | Queue frontier; visited markers | Detector in-tree; quality open |
| DFS | Recursion or explicit stack; visited marking | Detector in-tree; quality open |
| Dijkstra's | Min-heap + relaxation | Research-grade; not a product claim |

#### Numeric and Mathematical

| Algorithm | Key signal | Status |
|-----------|-----------|--------|
| Fast exponentiation | Shift exponent, square base, conditional multiply | Research |
| GCD (Euclidean / binary) | Repeated modulo or subtract | Research |
| FFT (Cooley-Tukey) | Bit-reversal; butterfly loops | Research |

Knapsack, LCS, and Fibonacci are **corpus labels only** (audit A6). No
structural detector assigns those kinds.

---

### Container Recovery

Detectors live under `src/container_detect`. Same honesty rule: present in
the tree; not a 1.0 product F1.

#### std::vector

**Internal layout**: `{begin*, end*, capacity_end*}`

- `push_back`: check `end < cap`, write to `*end`, increment `end`; on overflow, reallocate at 1.5× or 2×.
- Element stride is a type-parameter hint.

#### std::deque

**Internal layout**: block map + two-level index arithmetic.

#### std::list

**Internal layout**: doubly-linked list with circular sentinel.

#### std::map / std::set (Red-Black Tree)

**Node structure**: `{left*, right*, parent*, color, key[, value]}`

- Left/right rotations and rebalancing are the strongest signals.

#### std::unordered_map / std::unordered_set

**libstdc++ layout**: bucket array + chains. Hash on key; modulo bucket count.

#### std::priority_queue

Built on `push_heap` / `pop_heap` (same arithmetic as heapsort; context differs).

#### std::string (SSO)

SSO threshold check (15 libstdc++ / 22 libc++); inline vs heap.

#### std::shared_ptr / std::weak_ptr

Object pointer + control block; atomic strong/weak counts.

---

### Recovery Pipeline Architecture

```
Layer 1 — Structure Detection (bottom-up, per-function)     ← shipped detectors
    ↓  candidate matches + confidence scores + inferred parameters
Layer 2 — Context Validation (whole-program cross-check)    ← partial / open
    ↓  validated, high-confidence identifications
Layer 3 — Object Reconstruction (idiomatic STL emission)    ← open research
    ↓  v.push_back(), v.size(), map.find(), etc.
```

**Template type recovery** from comparator or hash remains research. Do not
claim Layer 3 as shipped.

---

## Part 2 — New Decompilation Targets

Aligned with [ARCHITECTURE_TARGETS.md](ARCHITECTURE_TARGETS.md) and the
v2.0.24 README. Native output is **C**. Other languages are **input-keyed**,
not a free-choice list.

### Architecture Targets

| Architecture | Status | Notes |
|-------------|--------|-------|
| x86-64 | **Production** | Primary native path |
| x86-32 | **Production** | — |
| ARM32 / Thumb | **Production** | Compiler VFP/NEON; leftover permute/crypto may be pseudo-asm |
| AArch64 | **Production** | Capstone + AAPCS64; scalar+vector FP; SVE/AES still unmapped |
| MIPS32/64 | **Production** | FPU + integer MUL/DIV; compact R6/microMIPS leftovers |
| PowerPC 32/64 | **Production** | Compiler FP + gcc -O1 VSX/Altivec; QPX unmapped |
| RISC-V (RV32I, RV64I) | **Production** | I+C+M+F+D+A+CSR; AMOMIN/MAX and fused FP leftover |
| LoongArch | Research | — |
| SPARC / SystemZ / XCore | **Production** | SPARC V8/V9 FP; SystemZ 64-only BFP; XCore 32-only (no channels) |
| WASM (binary) | **Shipped (input-keyed)** | `.wasm` → WAT |
| PTX (NVIDIA virtual ISA) | In-tree `src/ptx_decompile` | **Not** a general native output choice |
| SASS (NVIDIA machine code) | Library, not Production | cubin/fatbin loader + SM_70/80 subset in `src/sass_decode/`; no nvdisasm; not CLI |
| DXBC / DXIL (HLSL bytecode) | Future | — |
| SPIR-V | Future | — |

### Binary Format Targets

Loaders that accept input today (file opens ≠ native-lift maturity):

| Format | Status | Notes |
|--------|--------|-------|
| ELF (Linux) | **Shipped** | — |
| PE/PE+ (Windows) | **Shipped** | — |
| Mach-O (macOS/iOS) | **Shipped** | macOS `RetDec.app` is the GUI bundle, not a format |
| COFF / OMF | In-tree | — |
| .NET CIL / MSIL | **Shipped (managed path)** | C#-family emitter |
| JVM bytecode / DEX / APK | **Shipped (managed path)** | Java-family; APK resources not recovered |
| Python `.pyc` | **Shipped** | Python emitter |
| Lua `.luac` | **Shipped** | Lua emitter |
| WebAssembly `.wasm` | **Shipped** | WAT emitter |
| Fat binary / Universal | Future | macOS multi-arch containers |
| IPA (iOS) | Research | Mach-O + embedded frameworks |
| Swift ABI | Future | — |
| Rust ABI | Research | — |

### Output Language Targets

| Language | Status | Notes |
|----------|--------|-------|
| C | **Shipped default** | Native ELF/PE/Mach-O. `--buildable` on by default |
| C++ | **Rejected** | CLI rejects `--output-lang cpp`; `src/cxx_backend` unwired |
| Python | **Shipped** | From `.pyc` only |
| Lua | **Shipped** | From `.luac` only |
| WASM/WAT | **Shipped** | From `.wasm` only |
| Java | **Shipped (managed)** | From JVM / DEX |
| C# | **Shipped (managed)** | From CIL |
| CUDA C | In-tree, unwired as a free-choice target | From `.ptx` path only |
| F# / VB.NET / Kotlin | In-tree, unwired | Not a native free-choice target |
| Rust / Go / Swift / JS | Future / research | — |

---

## Part 3 — Research Frontiers

See [RESEARCH_FRONTIERS.md](RESEARCH_FRONTIERS.md) for the long-horizon list.
This section only records directions that overlap the product.

### CUDA and GPU Decompilation

**Not GPU *acceleration* of the decompiler.** `src/cuda_accel/` and
`src/opencl/` are parked research and are **not** linked from `src/retdec`.
`RETDEC_ENABLE_CUDA_ACCEL` defaults OFF. Docs: [CUDA_CAPABILITIES.md](CUDA_CAPABILITIES.md).

**PTX** as an *input* ISA is a separate, in-tree path (`src/ptx_decompile`).
**SASS** has a cubin/fatbin loader and a documented opcode subset in
`src/sass_decode/` (**not** Production, **not** in `decompile()`, no
`nvdisasm`). See [internal/wire-sass.md](internal/wire-sass.md).

### Machine Learning Integration

**Shipped, opt-in:** llama.cpp refine (`-DRETDEC_ENABLE_LLAMACPP=ON`,
`RETDEC_NEURAL_REFINE=1`, `RETDEC_NEURAL_MODEL`). GUI Tools → AI Assistant.
There is no in-tree `src/qwen3` (`C-QWEN3-GPU` withdrawn). Differential
neural gate is **not implemented**.

**Open research:** semantic validation via execution traces; neural algorithm
classification on IR features; confidence calibration regression.

### Structural Reconstruction

**Full project reconstruction** from a stripped binary is research. Achievable
research target: a CMake project grouped by call-graph clustering. Not shipped.

### Formal Verification, EH, Concurrency models, FFI, binary diffing

All remain research. The GUI Diff panel is a shipped viewer; cross-binary
diff, vulnerability pattern matching, and KLEE/angr integration are not.

### Performance Scalability

Type inference + structuring remain the suspected hot path. Treat wall-clock
ratios vs stock RetDec in Docker as **unmeasured** when build types differ
(Debug/WSL vs Release-in-Docker). Scale targets below are aspirations, not
v2.0.24 measurements.

**Research optimisations:** PGO, LTO across experimental OpenCL (parked),
parallel structuring, incremental per-function cache.

---

## Implementation Priority

Ordered by impact-to-effort. Items already in tree are marked; they are not
automatically “done”.

| Priority | Feature | Status at v2.0.24 |
|----------|---------|-------------------|
| 1 | STL container recovery (vector, map, unordered_map) | Detectors shipped; name-blind quality open |
| 2 | Sorting algorithm recovery (introsort, merge sort) | Detectors shipped; name-blind quality open |
| 3 | RISC-V support | Not implemented |
| 4 | Rust output language | Future |
| 5 | AI-assisted variable naming | **Opt-in llama.cpp shipped**; quality/tuning open |
| 6 | SASS decompilation (no nvdisasm) | Library subset in `src/sass_decode/`; not CLI / not Production |
| 7 | Cross-binary diff | Research (GUI Diff is same-session) |
| 8 | Formal verification bridge | Research |
