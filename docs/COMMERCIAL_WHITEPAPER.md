# RetDec Imortek — Specification Extraction Decompiler

**Commercial overview and technical summary**
*For readers new to reverse engineering, binary analysis, and decompilation.*
**Version 2.0.22** (`CMakeLists.txt`). Repo:
[odin-loki/RetDec-Decompiler](https://github.com/odin-loki/RetDec-Decompiler).
Release tag [v2.0.22](https://github.com/odin-loki/RetDec-Decompiler/releases/tag/v2.0.22).

This document must not outrun [CLAIMS.md](CLAIMS.md). Do not treat it as
evidence of unpublished F1 or GPU-pipeline numbers.

---

## Executive summary

**RetDec Imortek** is a **specification-extraction platform that contains a
decompiler**: it recovers algorithms, containers, concurrency patterns, and
structured semantics from compiled binaries, with optional offline neural
refinement. Human-readable C is a supporting artefact for review—not the
sole product metric. Other languages are emitted only on managed/bytecode
inputs, not as a free-choice native list.

This edition extends Avast RetDec 5.0 with semantic library recovery,
algorithm-recovery **name-blind** measurements, optional verified offline
neural refinement (llama.cpp), a Qt 6 desktop environment, and packaged
Linux / macOS / Windows installs.

---

## The problem this product addresses

Software is routinely distributed only as **binaries**: CPU instructions, platform-specific containers (ELF, PE, Mach-O), or virtual-machine bytecode (JVM, .NET, WASM, Python, Lua, Android DEX). That representation is optimized for execution, not for human reasoning. Teams face recurring needs to:

- **Recover intent** when source code is lost, incomplete, or under a different license.
- **Inspect closed or third-party components** for security, compliance, or integration.
- **Compare behavior** across builds or patches when only binaries are available.
- **Accelerate triage** in incident response by lifting logic into readable form.

A decompiler automates the hardest part of that workflow: translating low-level structure into structured source, while surfacing control flow, data types, and—where heuristics succeed—higher-level patterns such as standard-library usage or cryptographic routines.

---

## What “decompilation” means here (for newcomers)

1. **Loading** — Parse the file format, map sections into memory, resolve symbols, imports, and relocations where possible.
2. **Lifting** — Translate machine instructions (or bytecode) into an intermediate representation (IR) that is easier to analyze than raw opcodes.
3. **Analysis** — Build control-flow graphs (CFGs), infer types, recover functions, exceptions, RTTI, and calling conventions.
4. **Structuring** — Turn flat graphs into loops, branches, and structured control flow that resemble original source.
5. **Emission** — Print the result as **C** for native binaries. Managed
   formats emit the language of the input. `--buildable` sidecars (`.h`,
   `_stubs.c`, `.buildable.c`) are on by default.

RetDec’s design centers **LLVM IR as a pivot**: architecture-specific front ends lift to a common IR; middle and back ends are shared. Adding CPU support is largely a front-end concern. The tree pins LLVM **23.1.0** (`cmake/deps.cmake`). SPARC, SystemZ, and XCore lifting throw at create time and are unimplemented.

---

## Product positioning (Decision D7)

This product is positioned as **specification extraction**, not pseudocode
parity with Hex-Rays or Ghidra. Lead with:

- Buildable C (`--buildable`, default on) recompile **216/216** vs stock **0/216**
- Default `.c` recompile **216/216** on the same stand-in corpus (CC-01)
- Algorithm-recovery **name-blind** F1 **0.056** (95% CI 0.034–0.083)
- Semantic JSON export (`config.functions[].semanticDetections`)
- Offline neural refinement with a compile-only gate (`cc -fsyntax-only`;
  decompiled C is never executed). Differential gate is not implemented.

Do not advertise name-assisted F1 **1.0** as a product figure. Do not
advertise CUDA/OpenCL acceleration in the default pipeline (`C-CUDA-PIPE` withdrawn).

Pseudocode quality is reported honestly via DecompileBench; parity with stock
RetDec C output is expected and not marketed as the primary differentiator.
See [docs/internal/D7_DECISION.md](internal/D7_DECISION.md).

## What ships at 2.0.22

| Capability | Benefit |
|------------|---------|
| **Broad input formats** | One toolchain for native binaries, WASM, JVM/DEX, .NET, Python, and Lua artifacts. Native CPU maturity is Production (x86/x86-64) / Partial (ARM, Thumb, MIPS, PowerPC) / Incomplete (ARM64). SPARC / SystemZ / XCore / RISC-V are not implemented. |
| **Input-keyed output** | Native binaries emit C. Managed formats emit that format’s language. Not a free-choice eleven-language native list. |
| **Semantic recovery** | Post-pipeline detectors label containers, algorithms, crypto, concurrency, and serialisation in comments/JSON. They do not emit idiomatic C++. Name-blind F1 is 0.056. |
| **GPU backends (experimental)** | CUDA (`cuda_accel`) and OpenCL exist in-tree, default-OFF, **unintegrated**, not a product feature. |
| **On-device AI (opt-in)** | Optional GGUF via `RETDEC_NEURAL_REFINE` / `RETDEC_NEURAL_MODEL`. No `--model` flag. No `retdec-qwen3-runner`. |
| **Qt 6 GUI** | Document tabs, docks, Call Graph, Type Hierarchy, Signature Studio, Diff, Binary browser, AI Assistant Tools window. macOS ships `RetDec.app`. |
| **Plugins** | Example LLVM pass in `examples/decompiler_plugin/`. The decompiler CLI has no flag that loads a pass plugin. |
| **Packages** | Linux x86_64 tarball; macOS arm64 tarball + `RetDec.app`; Windows NSIS + portable zip (intended names; a given CI run may still be building). Keyless Sigstore. AppImage opt-in only. |

---

## Supported inputs (what you can feed the tool)

| Category | Examples |
|----------|----------|
| **Linux / Android native** | ELF (`.elf`, `.so`, `.o`) |
| **Windows native** | PE (`.exe`, `.dll`, `.sys`) |
| **Apple platforms** | Mach-O, `.dylib` |
| **Web** | WebAssembly (`.wasm`) |
| **JVM** | `.class`, `.jar` |
| **Android** | DEX, APK |
| **.NET** | Managed assemblies (`.dll` / CIL) |
| **Scripting runtimes** | Python bytecode (`.pyc`), Lua bytecode (`.luac`) |

A **managed-language dispatcher** routes JVM, DEX, CIL, Python, Lua, and WASM through dedicated parsers and emitters, **bypassing** the LLVM SSA pipeline where that path is more direct and accurate.

---

## Supported outputs (what you can obtain)

Native binaries emit **C**. Managed inputs emit the language of that format
(Python, Lua, WAT, Java-family, C#-family). F# / VB.NET / Kotlin / CUDA-C
writers in-tree are not general native-pipeline targets.

---

## Semantic recovery (why the output reads “smarter”)

Heuristic layers attempt to **name and structure** code in terms of familiar libraries and idioms, including:

- **Standard C++** — `std::vector`, `std::map`, `std::unordered_map`, `std::list`, `std::string`, `std::shared_ptr`, and related patterns.
- **Algorithms** — Sorting variants, binary search, graph traversals (BFS/DFS), and related algorithmic fingerprints. Fibonacci / LCS / Knapsack are **not** structural detections.
- **Cryptography** — AES, SHA family, ChaCha20, RSA, elliptic-curve primitives (detection-oriented, not a substitute for certified crypto review).
- **Concurrency** — `std::thread`, pthreads, Win32 threads, OpenMP, Intel TBB, atomics, spinlocks.
- **Serialization** — Protobuf, FlatBuffers, MessagePack, JSON, XML usage patterns.
- **C++ runtime** — Virtual tables, RTTI, constructors/destructors, exception handling reconstruction where metadata exists.

`CudaHostRecovery` is tests-only (not in `decompile()`). OpenCL host recovery
logs a summary. These detectors improve **analyst throughput**; they do not
guarantee bit-identical source recovery.

---

## Technical architecture (concise)

### Native pipeline (stages at a glance)

1. **Front end** — Load binary → disassemble to LLVM IR → CFGs → function boundaries → SSA → type inference → calling conventions → RTTI → exception metadata → pattern matching.
2. **Middle end** — Alias analysis, dead-code elimination, control-flow structuring, expression recovery, inter-procedural analysis. Post-pipeline SSA detectors (containers, algorithms, crypto, concurrency, serialisation) annotate config/comments. `src/module_cluster/` (Louvain) and `CudaHostRecovery` are **not** in this path.
3. **Semantic layers** — Detector labels plus optional `--buildable` sidecar. There is no C++ HLL writer and no automatic CMake project from clustering.
4. **Back end** — Language-specific codegen, demangling, inline recovery comments, formatting.

### Supporting libraries (examples)

- **`concurrency_detect`** — Pluggable detectors for std threads, pthreads, Win32, atomics, spinlocks, OpenMP, TBB.
- **`ptx_decompile`** — PTX parse/lift and CUDA-oriented recovery helpers (CPU).
- **`module_cluster`** — Community detection and downstream naming / `#include` inference / CMake emission.
- **`profiling`** — Stage timing, RSS tracking, CSV/JSON-style reporting for performance engineering.
- **`testing`** — In-memory stub binaries, snapshot regression helpers, harness utilities.

---

## GPU acceleration (experimental, not in the default pipeline)

In-tree **CUDA** (`src/cuda_accel`) and **OpenCL** (`src/opencl`) libraries are
**experimental and unintegrated**: they are not linked from `src/retdec` and
do not run during a default decompilation. `RETDEC_ENABLE_CUDA_ACCEL` is
**OFF** by default (including full CMake presets). Evaluators do not need an
NVIDIA card. Do not advertise GPU-backed analysis passes or automatic CPU
fallback as a shipped capability.

---

## AI assistant (on-device inference)

Optional on-device naming uses **llama.cpp + a GGUF on disk**
(`RETDEC_ENABLE_LLAMACPP` at build; `RETDEC_NEURAL_REFINE` at run). The
in-tree Qwen3/FlashAttention stack is **not wired into `src/retdec`**
(C-QWEN3-GPU withdrawn). Use `RETDEC_NEURAL_REFINE` and
`RETDEC_NEURAL_MODEL`. There is no `--model` CLI flag and no
`retdec-qwen3-runner`. The GUI Tools menu opens **AI Assistant**.

This is **optional**: core decompilation remains usable without any model on disk. Operation is **local-first**, which matters for air-gapped or data-sensitive environments—subject to your own policies on running third-party model weights.

---

## Qt 6 graphical application

The GUI targets a professional analyst workflow:

- **Tri-pane code view** — Synchronized assembly, SSA IR, and decompiled text.
- **CFG visualizer** — Basic blocks, Fit/zoom, Export SVG/PNG.
- **Type hierarchy and vtable** browsers (Tools windows).
- **Call graph** with SCC super-nodes.
- **Function list** with recovery confidence bars.
- **Strings and constants** browser.
- **Before/after diff** (Myers algorithm) for comparing outputs or stages.
- **AI Assistant** Tools window (`AIAssistantPanel`).
- **Settings** (Tools → Settings…): general, analysis, CUDA (persisted, not a pipeline), ML, recovery, advanced, decompiler CLI flags, plugins.

macOS ships **`RetDec.app`** in the arm64 tarball (ad-hoc signed, not
notarised). Linux AppImage is opt-in and often not on the tag.

---

## Command-line tools and automation

The primary CLI is **`retdec-decompiler`**, suitable for scripting and CI. Typical invocations decompile a file to a chosen output path; managed formats use the same entry point with automatic dispatch.

Additional tooling includes **`retdec-unpacker`**. There is no
`retdec-qwen3-runner` in staged Windows bundles. Exact tool availability
depends on **CMake options** and preset (see `docs/BUILD_REFERENCE.md`).

---

## Extensibility: plugin system

Shared libraries can implement documented interfaces, for example:

- **`IDecompilerPlugin`** — Hook the pipeline after built-in stages.
- **`IOutputPlugin`** — Transform or export decompiler text (**File → Export As**).
- **`IVisualisationPlugin`** — Add dockable panels.
- **`IAnalysisPlugin`** — Post-processing analysis passes.

Plugins are discovered and ordered with **dependency-aware loading**. Authoring guidance lives in `docs/developer_guide.md`.

---

## Build, deploy, and platform matrix

| Scenario | Highlights |
|----------|------------|
| **Linux / WSL (full)** | CMakeLists 3.13+; presets 3.26+. Qt 6 for GUI presets. CUDA accel opt-in and unintegrated. |
| **macOS** | Same `full-linux-*` presets (`build/linux/`). `ctest-macos` on arm64. Release tarball + `RetDec.app`. |
| **Windows (native full)** | MSVC, Qt 6; CUDA toolkit optional; staged `dist/windows/`. NSIS + zip intended Release names. |
| **Windows PE from Linux** | MinGW-w64 cross-build: CLI-focused bundle, no Qt in that path. |
| **Docker** | `docker-from-release.yml` packs the Linux tarball into GHCR (package may still be private). Hub `imortek/retdec` unpublished. In-tree `Dockerfile` is dispatch-only. |
| **Reduced footprint** | `core-*` presets for CLI-only trees without mandating Qt. |

Prerequisites and canonical commands are centralized in **`docs/BUILD_REFERENCE.md`** and the root **`README.md`**. Ten-minute install: **`QUICKSTART.md`**.

---

## Quality, testing, and observability

- **CTest**-driven unit and integration tests in the CMake build (`ctest-linux` / `ctest-macos` on push to `main`; `ctest-windows` nightly).
- LLVM-free **standalone-check**, parser **fuzz-pr**, and **verify-esbmc** gates.
- **Snapshot-style** regression helpers for stable textual outputs.
- **Profiling APIs** for wall-clock and resource measurement inside the codebase.
- **Diagnostic environment variables** (documented under `docs/README.md`).

Note: this tree does **not** clone an external regression corpus from GitHub
Actions secrets. In-tree CTest and `results/` JSON are the regression surface;
see `docs/BUILD_REFERENCE.md`.

---

## Limitations and expectations (honest scope)

No decompiler can **perfectly** recover original source for arbitrary optimized binaries. Obfuscation, stripped symbols, self-modifying code, novel packers, and aggressive interprocedural optimizations all reduce fidelity. RetDec mitigates this with broad format support, rich heuristics, visualization, and optional on-device AI assistance—but **human review** remains essential for high-stakes conclusions (e.g. legal evidence or mission-critical security verdicts).

Decompiling untrusted binaries is **intended use**. Parsers are not sandboxed;
see [THREAT_MODEL.md](THREAT_MODEL.md) and [../SECURITY.md](../SECURITY.md).

---

## Research and roadmap

Forward-looking topics are collected in **`docs/future_directions.md`**.
Shipped vs leftover packaging is in **[../ROADMAP.md](../ROADMAP.md)**.

---

## Licensing and attribution

Copyright **(c) 2025-2026 Odin Loch trading as Imortek**. This project is
**dual-licensed**:

1. **AGPL-3.0+** — [LICENSE-AGPL](../LICENSE-AGPL)
2. **Commercial** — [LICENSE-COMMERCIAL](../LICENSE-COMMERCIAL)

The root [LICENSE](../LICENSE) file points at both. Upstream RetDec v5.0 is
MIT ([LICENSE-MIT](../LICENSE-MIT)). **This white paper is descriptive
documentation**; it does not modify licence terms. Procurement questions:
[../LICENSING_FAQ.md](../LICENSING_FAQ.md). Enquiries:
**odin.loch@outlook.com.au**.

---

## Where to read next

| Document | Audience |
|----------|----------|
| [README.md](../README.md) | First-time orientation, feature matrix, measured results. |
| [QUICKSTART.md](../QUICKSTART.md) | Ten-minute decompile from Release assets. |
| [BUILD_REFERENCE.md](BUILD_REFERENCE.md) | Build engineers and packagers. |
| [architecture.md](architecture.md) | Deep pipeline and module reference. |
| [user_manual.md](user_manual.md) | End users of the Qt GUI. |
| [CLAIMS.md](CLAIMS.md) | What may be advertised. |
| [developer_guide.md](developer_guide.md) | Contributors and plugin authors. |

---

*End of document.*
