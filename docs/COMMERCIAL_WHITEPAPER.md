# RetDec — Enhanced Retargetable Decompiler

**Commercial overview and technical summary**  
*For readers new to reverse engineering, binary analysis, and decompilation.*

---

## Executive summary

**RetDec** is a machine-code decompiler: it takes compiled programs (executables, libraries, bytecode, and related artifacts) and reconstructs human-readable high-level source code. This repository is an **enhanced** edition of the RetDec lineage, built on a proven LLVM-oriented pipeline and extended with **semantic library recovery**, **multi-language output**, **optional CUDA GPU acceleration**, an **integrated large language model (LLM) assistant**, and a **Qt 6 desktop environment** for interactive analysis.

Typical uses include malware analysis, legacy software recovery, interoperability research, education, and audit workflows where understanding behavior without original source matters.

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
5. **Emission** — Print the result as **C**, **C++**, or another supported language, optionally annotated with recovery metadata.

RetDec’s design centers **LLVM IR as a pivot**: architecture-specific front ends lift to a common IR; middle and back ends are shared. Adding CPU support is largely a front-end concern.

---

## Product positioning: “Enhanced RetDec”

This tree is positioned as a **full-stack analysis and decompilation platform**, not a single-purpose “C-only” tool. Differentiators include:

| Capability | Benefit |
|------------|---------|
| **Broad input formats** | One toolchain for native binaries, GPU PTX, WASM, JVM/DEX, .NET, Python, and Lua artifacts. |
| **Multiple output languages** | Emit code in the language that best fits downstream tooling or analyst preference. |
| **Semantic recovery** | Recognize STL containers, algorithms, crypto, concurrency, serialization, and C++ runtime patterns—not only raw instructions. |
| **CUDA acceleration** | Optional GPU-backed passes with automatic CPU fallback when no suitable GPU is present. |
| **On-device AI** | Optional GGUF-based models (e.g. Qwen3 family) for naming, explanation, and interactive Q&A over recovered code—without mandatory cloud APIs. |
| **Qt 6 GUI** | IDE-style tri-pane views, CFG and call-graph visualization, diffing, and integrated assistant UI. |
| **Plugins** | Extend the pipeline, output, visualizations, or analysis via shared libraries. |

---

## Supported inputs (what you can feed the tool)

| Category | Examples |
|----------|----------|
| **Linux / Android native** | ELF (`.elf`, `.so`, `.o`) |
| **Windows native** | PE (`.exe`, `.dll`, `.sys`) |
| **Apple platforms** | Mach-O, `.dylib` |
| **NVIDIA GPU** | CUDA PTX (`.ptx`) |
| **Web** | WebAssembly (`.wasm`) |
| **JVM** | `.class`, `.jar` |
| **Android** | DEX, APK |
| **.NET** | Managed assemblies (`.dll` / CIL) |
| **Scripting runtimes** | Python bytecode (`.pyc`), Lua bytecode (`.luac`) |

A **managed-language dispatcher** routes JVM, DEX, CIL, Python, Lua, and WASM through dedicated parsers and emitters, **bypassing** the LLVM SSA pipeline where that path is more direct and accurate.

---

## Supported outputs (what you can obtain)

Beyond traditional **C** and **C++**, the product targets **Python**, **Lua**, **Java**, **Kotlin**, **C#**, **F#**, **Visual Basic .NET**, **WebAssembly text (WAT)**, and **CUDA C** (for PTX-oriented workflows). The exact fidelity depends on input class: native binaries pass through the full IR pipeline; bytecode formats use specialized lifters.

---

## Semantic recovery (why the output reads “smarter”)

Heuristic layers attempt to **name and structure** code in terms of familiar libraries and idioms, including:

- **Standard C++** — `std::vector`, `std::map`, `std::unordered_map`, `std::list`, `std::string`, `std::shared_ptr`, and related patterns.  
- **Algorithms** — Sorting variants, binary search, graph traversals (BFS/DFS), and related algorithmic fingerprints.  
- **Cryptography** — AES, SHA family, ChaCha20, RSA, elliptic-curve primitives (detection-oriented, not a substitute for certified crypto review).  
- **Concurrency** — `std::thread`, pthreads, Win32 threads, OpenMP, Intel TBB, atomics, spinlocks.  
- **CUDA host/device** — Host-side launch and memory patterns; PTX lifting toward CUDA C-style output.  
- **Serialization** — Protobuf, FlatBuffers, MessagePack, JSON, XML usage patterns.  
- **C++ runtime** — Virtual tables, RTTI, constructors/destructors, exception handling reconstruction where metadata exists.

These features improve **analyst throughput**; they do not guarantee bit-identical source recovery (no decompiler can promise that in the general case).

---

## Technical architecture (concise)

### Native pipeline (stages at a glance)

1. **Front end** — Load binary → disassemble to LLVM IR → CFGs → function boundaries → SSA → type inference → calling conventions → RTTI → exception metadata → pattern matching.  
2. **Middle end** — Alias analysis, dead-code elimination, control-flow structuring, expression recovery, inter-procedural analysis, concurrency and CUDA host recovery, PTX lifting, serialization detection, **Louvain-based module clustering** on the call graph.  
3. **Semantic layers** — STL/container recovery, algorithm recovery, crypto detection, C++ lifting, optional **CMake project emission** from inferred modules.  
4. **Back end** — Language-specific codegen, demangling, inline recovery comments, formatting.

### Supporting libraries (examples)

- **`concurrency_detect`** — Pluggable detectors for std threads, pthreads, Win32, atomics, spinlocks, OpenMP, TBB.  
- **`ptx_decompile`** — PTX parse/lift and CUDA-oriented recovery helpers.  
- **`module_cluster`** — Community detection and downstream naming / `#include` inference / CMake emission.  
- **`profiling`** — Stage timing, RSS tracking, CSV/JSON-style reporting for performance engineering.  
- **`testing`** — In-memory stub binaries, snapshot regression helpers, harness utilities.

---

## GPU acceleration (CUDA)

Analysis passes can use **CUDA** for workloads such as parallel disassembly, alias analysis, type inference, semantic hashing, and e-graph simplification. The implementation is designed for **practical deployment**: if no suitable NVIDIA stack is available, the same logical passes **fall back to multi-threaded CPU** implementations without manual reconfiguration.

For **maximum capability on Windows** (MSVC + CUDA + Qt 6 GUI), the project documents a **native Windows** build path; **Linux/WSL cross-compilation to Windows** produces a **CLI-oriented** bundle without CUDA or the Qt GUI, reflecting toolchain constraints (GPU kernels are not cross-compiled in that mode).

---

## AI assistant (on-device inference)

The tree integrates a **Qwen3-oriented** inference stack (Mixture-of-Experts capable), with **FlashAttention-2**-style optimizations and **GPU acceleration** where enabled. Analysts can load **GGUF-quantized** weights (for example from an Ollama pull copied into a `models/` directory) for:

- Explanations and naming suggestions over decompiled output.  
- Interactive chat in the GUI (**streaming** tokens from a worker thread).  
- CLI-assisted workflows (e.g. `--model` on `retdec-decompiler`).

This is **optional**: core decompilation remains usable without any model on disk. Operation is **local-first**, which matters for air-gapped or data-sensitive environments—subject to your own policies on running third-party model weights.

---

## Qt 6 graphical application

The GUI targets a professional analyst workflow:

- **Tri-pane code view** — Synchronized assembly, SSA IR, and decompiled text with shared line mapping.  
- **CFG visualizer** — Basic blocks, conditional edge styling, loop highlighting, minimap.  
- **Type hierarchy and vtable** browsers.  
- **Call graph** with SCC “super-nodes” and module cluster overlays.  
- **Function list** with recovery confidence cues.  
- **Strings and constants** browser with semantic classification.  
- **Before/after diff** (Myers algorithm) for comparing outputs or stages.  
- **AI assistant** panel with streaming responses.  
- **Settings** across multiple tabs (general, analysis, ML model, recovery toggles, advanced diagnostics, plugins).  
- **Plugin manager** integration for third-party extensions.

---

## Command-line tools and automation

The primary CLI is **`retdec-decompiler`**, suitable for scripting and CI. Typical invocations decompile a file to a chosen output path; managed formats use the same entry point with automatic dispatch.

Additional tooling includes **`retdec-unpacker`** (packed / archive-oriented unpacking in the broader RetDec ecosystem) and, where built, auxiliary binaries such as an **AI runner** in staged Windows bundles. Exact tool availability depends on **CMake options** and preset (see `docs/BUILD_REFERENCE.md`).

---

## Extensibility: plugin system

Shared libraries can implement documented interfaces, for example:

- **`IDecompilerPlugin`** — Hook the pipeline after built-in stages.  
- **`IOutputPlugin`** — Transform or export decompiler text.  
- **`IVisualisationPlugin`** — Add dockable panels.  
- **`IAnalysisPlugin`** — Post-processing analysis passes.

Plugins are discovered and ordered with **dependency-aware loading** (topological sort). Authoring guidance lives in `docs/developer_guide.md`.

---

## Build, deploy, and platform matrix

| Scenario | Highlights |
|----------|------------|
| **Linux / WSL (full)** | CMake 3.26+, Qt 6 for GUI presets, optional CUDA for acceleration. |
| **Windows (native full)** | MSVC, CUDA, Qt 6; scripts for dependency install, configure, build, and smoke tests; staged `dist/windows/` layout. |
| **Windows PE from Linux** | MinGW-w64 cross-build: CLI-focused bundle, no Qt/CUDA in that path. |
| **Docker** | Dockerfile in-repo for containerized builds (see docs). |
| **Reduced footprint** | “Core” style presets for faster CLI-only trees without mandating Qt. |

Prerequisites and canonical commands are centralized in **`docs/BUILD_REFERENCE.md`** and the root **`README.md`**.

---

## Quality, testing, and observability

- **CTest**-driven unit and integration tests in the CMake build.  
- **Snapshot-style** regression helpers for stable textual outputs.  
- **Profiling APIs** for wall-clock and resource measurement inside the codebase.  
- **Diagnostic environment variables** (documented under `docs/README.md`) for deep pipeline logging when investigating edge cases.

Note: full **external regression corpora** may require **CI secrets** configured in GitHub Actions (`RETDEC_REGRESSION_TESTS_GIT_URL`, `RETDEC_REGRESSION_FRAMEWORK_GIT_URL`); see `docs/BUILD_REFERENCE.md`.

---

## Limitations and expectations (honest scope)

No decompiler can **perfectly** recover original source for arbitrary optimized binaries. Obfuscation, stripped symbols, self-modifying code, novel packers, and aggressive interprocedural optimizations all reduce fidelity. RetDec mitigates this with broad format support, rich heuristics, visualization, optional GPU acceleration, and AI assistance—but **human review** remains essential for high-stakes conclusions (e.g. legal evidence or mission-critical security verdicts).

---

## Research and roadmap

Forward-looking topics—additional recovery targets, academic-style open problems, and pipeline research—are collected in **`docs/future_directions.md`**.

---

## Licensing and attribution

Copyright **(c) 2025 Odin Loch Trading as Imortek**. Redistribution terms are given in the root **`LICENSE`** file (BSD-style license with attribution and no-endorsement clauses). **This white paper is descriptive documentation**; it does not modify license terms. For commercial distribution, bundle the license and required notices with binaries per the license text.

---

## Where to read next

| Document | Audience |
|----------|----------|
| [README.md](../README.md) | First-time orientation, feature matrix, quick start. |
| [BUILD_REFERENCE.md](BUILD_REFERENCE.md) | Build engineers and packagers. |
| [architecture.md](architecture.md) | Deep pipeline and module reference. |
| [user_manual.md](user_manual.md) | End users of the Qt GUI. |
| [developer_guide.md](developer_guide.md) | Contributors and plugin authors. |

---

*End of document.*
