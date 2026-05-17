# RetDec Architecture Guide

This document describes the complete architecture of the RetDec enhanced
decompiler: every pipeline stage, all library modules, the Qt GUI subsystem,
the AI inference engine, and the plugin system.

**Companion docs:** [BUILD_REFERENCE.md](BUILD_REFERENCE.md) (how to compile and lay out `build/` trees) · [developer_guide.md](developer_guide.md) (contributing, tests, style) · [user_manual.md](user_manual.md) (GUI) · [docs/README.md](README.md) (index).

---

## Table of Contents

1. [High-Level Overview](#overview)
2. [Build System and Module Layout](#modules)
3. [Full Pipeline Stage Reference](#pipeline)
4. [Core Library Details](#libraries)
5. [Qt GUI Architecture](#gui)
6. [AI Inference Engine (Qwen3)](#ai)
7. [Plugin System](#plugins)
8. [Performance and Threading Model](#threading)
9. [Design Decisions](#decisions)
10. [Data Flow Diagrams](#diagrams)

---

## High-Level Overview {#overview}

RetDec is a retargetable machine-code decompiler.  Given a binary (ELF, PE,
Mach-O, CUDA, WASM, JVM, DEX, `.pyc`, `.luac`, CIL), it produces human-readable
source code in C, C++, Python, Lua, Java, Kotlin, C#, F#, VB.NET, or WAT.

The enhanced version adds:

- **Semantic recovery**: STL containers, cryptographic primitives, concurrency
  synchronisation, CUDA host/device code, serialisation frameworks (Protobuf,
  FlatBuffers, JSON, XML).
- **Multiple output languages**: nine target languages beyond plain C.
- **AI assistant**: integrated Qwen3 MoE LLM for interactive analysis,
  streaming over a background thread.
- **Qt 6 GUI**: multi-panel IDE-style interface with CFG visualiser, type
  hierarchy browser, call graph explorer, diff view, strings browser, and
  settings/plugin system.
- **Performance harness**: wall-clock profiling, OpenCL kernel timing, RSS
  tracking, CSV/JSON reports.
- **Testing infrastructure**: `TestBinary` stub builder, snapshot regression
  testing, corpus runner, performance asserter, mock pipeline.

---

## Build System and Module Layout {#modules}

The project uses CMake **3.26+** (see root `CMakePresets.json`) with per-library `CMakeLists.txt` files.
Dependencies are managed via vcpkg (`vcpkg.json`).

```
retdec-master/
├── CMakeLists.txt            Root superbuild
├── CMakePresets.json         debug / release / asan / tsan presets
├── vcpkg.json                Dependency manifest
├── include/retdec/           All public headers
│   ├── concurrency_detect/   Concurrency/synchronisation detector
│   ├── module_cluster/       Louvain module clustering + CMake generation
│   ├── profiling/            Performance profiling harness
│   ├── ptx_decompile/        PTX parser + CUDA C lifter
│   ├── qwen3/                Tokeniser, weights, FlashAttn, MoE, pipeline
│   ├── testing/              Test harness utilities
│   └── gui/
│       ├── panels/           All Qt panel widgets
│       └── settings/         AppSettings, PluginManager, plugin interfaces
├── src/                      Implementations (mirrors include/)
├── tests/                    Unit + integration tests
└── docs/                     Documentation
```

### Library dependency graph

Dependencies flow strictly downward; no cycles.

```
retdec-gui
  ├── retdec-qwen3          (AI inference pipeline)
  ├── retdec-profiling      (standalone — no deps)
  ├── retdec-module-cluster (standalone)
  ├── retdec-concurrency-detect (standalone)
  ├── retdec-ptx-decompile  (standalone)
  └── Qt6::Widgets/Core/Gui

retdec-testing              (standalone — test helpers only)
retdec-qwen3                (standalone)
retdec-profiling            (standalone)
```

---

## Full Pipeline Stage Reference {#pipeline}

### Front-End (Stages 1–10): Binary → SSA IR

| Stage | Library | Input | Output |
|-------|---------|-------|--------|
| 1 Binary Loader | `fileformat` | Raw bytes | Sections, symbols, imports, relocations |
| 2 Disassembler | `capstone2llvmir` | Sections | LLVM IR basic blocks |
| 3 CFG Construction | `cfg` | LLVM IR | Per-function CFGs |
| 4 Function Boundary | `func_boundary` | CFGs | Confirmed function list |
| 5 SSA Lifting | `ssa` | CFGs | SSA IR (`PhiNode`, `SSAValue`) |
| 6 Type Inference | `type_inference` | SSA IR | Type annotations |
| 7 Calling Convention | `call_conv` | Functions | CC descriptors (SysV, Win64, ARM AAPCS) |
| 8 RTTI Recovery | `rtti` | SSA IR + symbols | C++ class hierarchy |
| 9 EH Reconstruction | `eh_reconstruct` | DWARF / `.pdata` | `try`/`catch`/`finally` blocks |
| 10 Pattern Matching | `pattern_detect` | SSA IR | Library call identifications |

**Key design choice — LLVM IR as pivot**: all front-ends (x86, ARM, MIPS,
PowerPC) produce LLVM IR.  All back-ends consume LLVM IR.  Adding a new
architecture means adding only a new lifting front-end; the entire middle-end
and back-end is shared.

### Middle-End (Stages 11–20): SSA IR → Language AST

| Stage | Library | Purpose |
|-------|---------|---------|
| 11 Alias Analysis | `alias_analysis` | Points-to sets, must/may alias |
| 12 Dead Code Elimination | `dce` | Remove unreachable and unused SSA defs |
| 13 Control Flow Structuring | `cfg_structure` | Recover `if/else`, `while`, `for`, `switch` |
| 14 Expression Recovery | `var_recovery` | Simplify SSA → C expressions |
| 15 IPA | `ipa` | Inter-procedural summary propagation |
| 16 Concurrency Detection | `concurrency_detect` | Mutexes, threads, atomics, OpenMP, TBB |
| 17 CUDA Host Recovery | `ptx_decompile` | `cudaLaunchKernel`, memory ops, streams |
| 18 PTX Lifting | `ptx_decompile` | PTX assembly → CUDA C AST |
| 19 Serialisation Detection | `serial_detect` | Protobuf, FlatBuffers, JSON, XML patterns |
| 20 Module Clustering | `module_cluster` | Louvain community detection on call graph |

### Semantic Recovery (Stages 21–25): IR + Metadata → Annotated AST

| Stage | Library | Purpose |
|-------|---------|---------|
| 21 STL/Container Recovery | `container_detect` | std::vector, map, list, string, etc. |
| 22 Algorithm Recovery | `algo_recover` | sort, binary search, BFS/DFS, FFT |
| 23 Crypto Detection | `crypto_detect` | AES, SHA, RSA, ChaCha20 implementations |
| 24 C++ Lifting | `cxx_backend` | vtables, new/delete, constructors, namespaces |
| 25 CMake Generation | `module_cluster` | Emit `CMakeLists.txt` from module graph |

### Back-End (Stages 26–29): AST → Source Code

| Stage | Library | Purpose |
|-------|---------|---------|
| 26 Language Emission | `codegen` | Dispatch to language-specific emitter |
| 27 Name Demangling | `demangler` | C++/D/Rust/Swift symbol demangling |
| 28 Comment Insertion | `codegen` | Recovery metadata as inline comments |
| 29 Formatter | `codegen` | clang-format style application |

### Managed / Interpreted Language Paths

These bypass the SSA pipeline entirely, operating on their own bytecode:

| Input | Lifter | Emitter |
|-------|--------|---------|
| JVM `.class` / `.jar` | `jvm_parser` + `jvm_reconstruct` | `java_emitter`, `kotlin_emitter` |
| Android `.dex` / `.apk` | `dex_parser` | `java_emitter`, `kotlin_emitter` |
| .NET CIL / `.dll` | `cli_parser` + `cil_reconstruct` | `csharp_emitter`, `vbnet_emitter` |
| Python `.pyc` | `pyc_parser` + `py_reconstruct` | `py_emitter` |
| Lua `.luac` | `lua_parser` | `lua_emitter` |
| WASM `.wasm` | `wasm_parser` | `wat_emitter` |

---

## Core Library Details {#libraries}

### `concurrency_detect`

Seven detector classes, all implementing `IConcurrencyDetector`:

| Class | Detects |
|-------|---------|
| `StdThreadDetector` | `std::thread`, `std::mutex`, `std::condition_variable` |
| `PthreadDetector` | `pthread_create`, `pthread_mutex_*`, `pthread_cond_*`, semaphores |
| `Win32ThreadDetector` | `CreateThread`, `WaitForSingleObject`, CRITICAL_SECTION, events |
| `AtomicDetector` | `std::atomic<T>`, `__atomic_*` builtins, `LOCK XCHG` patterns |
| `SpinlockDetector` | Compare-and-swap loops, `__sync_bool_compare_and_swap` |
| `OpenMPDetector` | `__kmpc_fork_call`, `omp_get_thread_num`, parallel region entry/exit |
| `TBBDetector` | `tbb::parallel_for`, `tbb::task_group`, `tbb::concurrent_vector` |

Results stored in `ConcurrencyModel`.  `ConcurrencyEmitter` produces a
human-readable summary.  `ConcurrencyDetector` orchestrates all sub-detectors.

### `ptx_decompile`

**`PtxParser`**: tokenises PTX text; parses `.target`, `.entry`, `.func`,
`.reg`, `.shared`, `.local`, `.param` directives; produces `PtxModule`.

**`InstrLifter`**: maps 30+ PTX instruction types to CUDA C:
`mov`, `add`, `mul`, `ld`, `st`, `setp`, `selp`, `cvt`, `bar.sync`,
`membar`, `atom`, `vote`, `shfl`, `sqrt`, `bra`, labels, `ret`.

**`ThreadIndexRecovery`**: maps PTX special registers:
- `%tid.{x,y,z}` → `threadIdx.{x,y,z}`
- `%ctaid.{x,y,z}` → `blockIdx.{x,y,z}`
- `%ntid.{x,y,z}` → `blockDim.{x,y,z}`
- `%nctaid.{x,y,z}` → `gridDim.{x,y,z}`
- `%laneid` → `threadIdx.x % 32`
- `%warpid` → `threadIdx.x / 32`

**`CudaHostRecovery`**: five detector classes for CUDA Runtime and Driver API:
`KernelLaunchDetector`, `CudaMemoryDetector`, `CudaDeviceDetector`,
`CudaStreamEventDetector`, `NvccStubDetector`.

### `module_cluster`

**`LouvainClusterer`**: iterative modularity maximisation.

Modularity formula:
```
Q = (1/2m) Σ_{ij} [A_{ij} − γ·k_i·k_j/2m] δ(c_i, c_j)
```

Node-moving complexity: O((N+M)·D) where D ≤ 20 passes in practice.

Post-Louvain refinements:
1. **String locality**: merge communities sharing string-pool references.
2. **Debug symbols**: merge communities where functions share a `sourceFile`.
3. **RTTI clustering**: merge classes with shared typeinfo into one module.
4. **Symbol prefix**: merge functions with a common demangled namespace prefix.

**`ModuleNamer`**: heuristics for module names:
- Common symbol prefix (demangled namespace)
- Library call fingerprint (e.g., functions calling `SSL_*` → "crypto")
- Source file name from debug info

**`HeaderInference`**: maps library symbol sets to `#include` directives.
Covers: stdlib, stdio, POSIX, OpenSSL, CUDA, pthreads, WinAPI, Qt6.

**`CMakeEmitter`**: generates `CMakeLists.txt` with `add_library`, `add_executable`,
`find_package`, `target_link_libraries`, install rules, and optional CTest.

### `profiling`

See [algorithm_reference.md](algorithm_reference.md) for the online softmax
algorithm.  Key implementation points:

- All public methods are thread-safe via `std::mutex`.
- `Profiler::time(name, fn)` is a zero-overhead template when disabled.
- `RssTracker` is platform-specific:
  - Linux: parses `/proc/self/status` `VmPeak:` line.
  - macOS: `getrusage(RUSAGE_SELF)` + `mach_task_basic_info` for current RSS.
  - Windows: `GetProcessMemoryInfo` (PSAPI).
- `FunctionHistogram` stores up to 10,000 raw samples for percentile computation,
  falling back to bucket counts beyond that limit.

### `testing`

`TestBinary` serialises valid (but trivially empty) binaries:

- **ELF64**: 64-byte Ehdr + 56-byte PT_LOAD phdr + sections + section headers.
  ELF magic `0x7F 'E' 'L' 'F'`, class byte 2 (64-bit), little-endian.
- **ELF32**: same with 32-bit fields and class byte 1.
- **PE32**: DOS MZ stub + PE signature + minimal COFF header + optional header
  with 16 zeroed data directories.
- **Raw**: no header; just section data concatenated.

`SnapshotTester` uses FNV-1a 64-bit as its hash function — non-cryptographic,
single-pass, ~1 byte/cycle, no dependencies.

---

## Qt GUI Architecture {#gui}

### Window Layout

`MainWindow` (QMainWindow) hosts all panels as `QDockWidget`s.

```
MainWindow
  ├── TriPaneCodeView          [central widget]
  ├── FunctionListPanel        [left dock]
  ├── TypeHierarchyPanel       [right dock, tabbed]
  ├── CallGraphPanel           [right dock, tabbed]
  ├── CFGPanel                 [bottom dock, tabbed]
  ├── StringsBrowserPanel      [bottom dock, tabbed]
  ├── AIAssistantPanel         [bottom dock, tabbed]
  ├── ProgressPanel            [bottom status dock]
  └── DiffPanel                [on demand, floating]
```

All panels inherit `PanelBase : QWidget`, which adds:
- `title()` — used as dock widget title.
- `clear()` — resets panel to empty state.
- `setActiveFunction(const QString&)` — called when the selected function changes.

### TriPaneCodeView

Three `SyncedCodePane` instances (each a `QPlainTextEdit` subclass) share a
`LineMapping` that maps source lines to assembly addresses and SSA IR nodes.
Scroll synchronisation uses `QScrollBar::valueChanged` with a re-entrancy guard.

`CodeSyntaxHighlighter` implements `QSyntaxHighlighter` with keyword/operator/
string/comment rules for C, assembly, and SSA IR dialects.

### CFGPanel

`CFGScene` (QGraphicsScene) + `CFGView` (QGraphicsView):

- `BasicBlockItem`: rounded-rect with opcode text; highlighted on selection.
- `CFGEdgeItem`: arrow with true/false colour coding for conditional branches.
- `LoopRegionItem`: semi-transparent background overlay for back-edge loops.
- `MiniMapView`: scaled thumbnail of the full scene with a viewport rectangle.

### TypeHierarchyPanel + CallGraphPanel

`ClassHierarchyModel` (QAbstractItemModel) drives a `QTreeView`.
`VtableModel` (QAbstractTableModel) drives a `QTableView`.

`CallGraphScene` uses `SccSuperNodeItem` for strongly-connected components
and `ModuleClusterItem` (coloured background region) for Louvain modules.

### AIAssistantPanel

Threading model:
```
Main Thread                   Worker Thread (QThread)
    │                               │
    │── startInferenceRequest ──►  InferenceWorker::startInference()
    │                               │  pipeline_->generate(prompt, callback)
    │◄── tokenGenerated(token) ────◄│  callback: emit tokenGenerated (queued)
    │◄── inferenceComplete() ──────◄│
    │
    onTokenGenerated: append to HTML chat bubble
```

`InferenceWorker` holds an `std::atomic_bool abort_` which is set by
`abortInference()`.  The generation callback checks this flag and returns
`false` (stop) when set.

### SettingsDialog

Seven-tab `QDialog` backed by `AppSettings`:

| Tab | Settings struct |
|-----|-----------------|
| General | `GeneralSettings` — theme, font, language |
| Analysis | `AnalysisSettings` — stage toggles, thresholds, threads |
| OpenCL | `OpenCLSettings` — device, cache dir, profiling |
| ML | `MLSettings` — model path, quantisation, temperature |
| Recovery | `RecoverySettings` — per-detector toggles and thresholds |
| Advanced | `AdvancedSettings` — verbosity, IR dump, intermediate output |
| Plugins | `PluginSettings` + live `PluginManager` interaction |

`AppSettings` persists to `~/.config/retdec/settings.ini` on Linux/macOS
and to `HKCU\Software\retdec\settings.ini` on Windows (INI format forced
on all platforms for portability and diff-friendliness).

### Plugin System

```
Plugin file (.so / .dll)
  exports:
    retdec_create_plugin()      → IRetDecPlugin*
    retdec_destroy_plugin(p)
    retdec_plugin_api_version() → const char*  (must == "1.0")
```

`PluginManager` loads plugins with `QPluginLoader`, verifies the API version,
calls `initialize()`, and inserts into a topologically sorted list (Kahn's
algorithm on declared dependencies).

Plugin types:

| Interface | Hook |
|-----------|------|
| `IDecompilerPlugin` | `runStage(PipelineContext&)` — runs after all built-in stages |
| `IOutputPlugin` | `transform(decompiledC)` — new export format |
| `IVisualisationPlugin` | `createPanel(parent)` — new dockable panel |
| `IAnalysisPlugin` | `analyse(PipelineContext&)` — post-processing pass |

---

## AI Inference Engine (Qwen3) {#ai}

### Component Stack

```
Qwen3Pipeline
  ├── Qwen3Tokenizer    BPE tokeniser + ChatML templating
  ├── Qwen3Weights      GGUF/SafeTensors loader + quantised tensor views
  ├── Qwen3FlashAttn    FlashAttention-2 (OpenCL GPU + CPU fallback)
  ├── Qwen3MoeLayer     MoE router + dispatcher + shared expert
  └── Qwen3Sampler      Temperature, top-P, top-K, repetition penalty
```

### Qwen3Tokenizer

- Byte-level pre-tokenisation with regex split.
- BPE merge table loaded from GGUF metadata.
- ChatML template:
  ```
  <|im_start|>system\n{system_prompt}\n<|im_end|>\n
  <|im_start|>user\n{user_message}\n<|im_end|>\n
  <|im_start|>assistant\n
  ```
- Special tokens: `<|im_start|>` = 151644, `<|im_end|>` = 151645.

### Qwen3Weights

Supports two weight formats:
- **GGUF**: reads header metadata, tensor descriptors, then mmap-maps data.
  Supports dtypes: `GGUF_F32`, `GGUF_F16`, `GGUF_Q4_0`, `GGUF_Q4_K_M`,
  `GGUF_Q5_K_M`, `GGUF_Q6_K`.
- **SafeTensors**: JSON header + raw tensor data, memory-mapped.

### FlashAttention-2

See [algorithm_reference.md](algorithm_reference.md) for the full mathematical
treatment.  Implementation notes:

- Block size constants: `FLASH_BR = 64` (row tiles), `FLASH_BC = 64` (col tiles).
- **Dense KV cache** (`KvCacheLayer`): pre-allocated `float*` buffers for K and V;
  indexed by sequence position.
- **Paged KV cache** (`PagedKvCache`): fixed-size blocks (default 16 tokens);
  block table maps `(layer, slot) → block_id`; supports variable-length
  sequences without memory fragmentation.
- **OpenCL kernel**: embedded as a `const char*` string in `qwen3_attention.cpp`;
  compiled at runtime by `clCreateProgramWithSource`.  Falls back to
  `flashAttnCpu` if no OpenCL device is available.

### MoE Layer

- **Router**: `W_g` weight matrix; softmax gating; `std::nth_element` for O(E)
  top-K selection.
- **Dispatcher**: evaluates K selected experts (SwiGLU FFN each); weighted sum.
- **Shared expert**: always active; result added unconditionally.
- **Load monitor**: per-expert activation fraction; alerts on hot/cold imbalance.

### Qwen3Pipeline

Full inference loop:

1. `embedToken(id)` → look up embedding row.
2. For each layer: `runLayer(layer_idx, pos)`:
   - RMSNorm on input.
   - QKV projections.
   - RoPE application via `ropeApplyAll`.
   - `Qwen3FlashAttn::forward` (or `forwardPaged`).
   - Output projection.
   - Residual add.
   - FFN: `Qwen3MoeLayer::forward` or dense SwiGLU.
   - Second residual add.
3. Final RMSNorm + LM head projection → logits.
4. `Qwen3Sampler::sample(logits)` → next token ID.
5. Repeat until EOS or `max_new_tokens`.

---

## Performance and Threading Model {#threading}

### Analysis Pipeline Threads

- Analysis stages run on `QThreadPool::globalInstance()`.
- Functions are independent; structuring and expression recovery run per-function
  in parallel across all hardware threads.
- `AnalysisBridge` coordinates stage sequencing and emits progress signals to
  `ProgressPanel`.

### AI Inference Thread

- `InferenceWorker` lives on a dedicated `QThread`.
- Generation callbacks use `Qt::QueuedConnection` to marshal `tokenGenerated`
  signals back to the main thread.
- `abort_` is `std::atomic_bool` — set by any thread, checked by the worker.

### OpenCL Concurrency

- OpenCL command queues are per-device.
- `cl_event` objects are used for kernel timing when profiling is enabled.
- The FlashAttention kernel is dispatched asynchronously; results are read back
  with `clEnqueueReadBuffer` + blocking wait.

### Profiling Overhead

`Profiler::measure(name)` takes one `std::mutex` lock on construction and one
on destruction.  On a modern CPU this is approximately 20–50 ns per scope — well
below the granularity of any pipeline stage.

When disabled (`setEnabled(false)`), `ScopeTimer` is a no-op: the constructor
stores the name and start time, but the destructor skips the lock and record.

---

## Design Decisions {#decisions}

### Why LLVM IR as the pivot format?

LLVM IR provides a stable, well-defined semantics for all supported architectures.
The entire middle-end (alias analysis, DCE, structuring, expression recovery)
operates on LLVM IR regardless of the input architecture.  This maximises code
reuse and separates front-end correctness from back-end quality.

The alternative (architecture-specific IRs, as in older decompilers) requires
reimplementing every middle-end pass per architecture.

### Why Louvain for module clustering?

Louvain achieves near-linear time O((N+M)·D) on call graphs with tens of
thousands of nodes.  The resolution parameter γ provides a tunable trade-off
between many small modules and few large ones.  Alternative approaches
(spectral clustering, k-means on call vectors) are slower and less
interpretable.

The post-processing refinements (string locality, debug symbols) are necessary
because the call graph alone is an impoverished signal for code organisation:
utility functions called by many modules will be placed in the largest
community, which is not always the most meaningful grouping.

### Why Myers diff in the DiffPanel?

Myers produces the *minimum edit script* in O((N+M)·D) time.  For typical
decompiler output changes (a few hundred lines), this is sub-millisecond.
The Hirschberg divide-and-conquer variant reduces space from O(ND) to O(N+M),
which matters for large functions.

The minimum edit script produces the cleanest visual diff — fewer spurious
changes than greedy or heuristic approaches.

### Why FNV-1a for snapshot hashing?

FNV-1a is:
- **Fast**: ~1 byte/cycle, no hardware acceleration needed.
- **Dependency-free**: 10 lines of code.
- **Non-cryptographic**: collision resistance is not needed for regression testing.

SHA-256 would add a dependency, be ~10× slower, and provide no practical
benefit for the use case of detecting unintentional output changes.

### Why INI format for settings?

INI is human-readable, diff-friendly, and portable.  Settings files can be
committed to version control for team configuration sharing.  JSON would also
work but requires a JSON parser dependency.  QSettings handles INI natively.

### Why a custom Qwen3 inference engine?

For pure inference of a public model, llama.cpp already runs Qwen3 models
efficiently.  The reasons to build a self-contained engine are:

1. **Auditability**: for defence and research applications, a clean-room
   implementation with full source visibility is required.
2. **Direct integration**: the pipeline connects `Qwen3Pipeline::generate()`
   directly to the decompiler context without IPC or subprocess overhead.
3. **Custom attention patterns**: paged KV cache with block-table management
   and FlashAttention-2 are tuned for the specific workloads of interactive
   code analysis (short bursts, moderate context lengths).
4. **No external dependencies at runtime**: the inference engine has zero
   shared-library dependencies beyond the C++ standard library and OpenCL.

---

## Data Flow Diagrams {#diagrams}

### Full Pipeline

```
Binary File
    │
    ▼
┌──────────────────────────────────────────────────────┐
│  Stage 1–5: Front-End                                │
│  Loader → Disasm → CFG → Func Boundary → SSA         │
└─────────────────────────┬────────────────────────────┘
                          │  SSA IR
                          ▼
┌──────────────────────────────────────────────────────┐
│  Stage 6–10: Type & Pattern Recovery                 │
│  Types → CC → RTTI → EH → Patterns                  │
└─────────────────────────┬────────────────────────────┘
                          │  Annotated SSA IR
                          ▼
┌──────────────────────────────────────────────────────┐
│  Stage 11–15: Middle-End Optimisation                │
│  Alias → DCE → Structuring → Expr → IPA             │
└─────────────────────────┬────────────────────────────┘
                          │  Structured AST
                          ▼
┌──────────────────────────────────────────────────────┐
│  Stage 16–25: Semantic & Structural Recovery         │
│  Concurrency → CUDA → Serial → Modules               │
│  STL → Algorithms → Crypto → C++ Lift → CMake       │
└─────────────────────────┬────────────────────────────┘
                          │  Language AST
                          ▼
┌──────────────────────────────────────────────────────┐
│  Stage 26–29: Back-End Emission                      │
│  Language Emit → Demangle → Comments → Format        │
└─────────────────────────┬────────────────────────────┘
                          │
                 Final Source Code
                 (C / C++ / Python / Java / Kotlin /
                  C# / F# / VB.NET / Lua / WAT)
```

### Managed Language Paths (bypass SSA)

```
JVM .class/.jar  →  jvm_parser → jvm_reconstruct → java_emitter / kotlin_emitter
Android .dex     →  dex_parser →                 → java_emitter / kotlin_emitter
.NET CIL         →  cli_parser → cil_reconstruct → csharp_emitter / vbnet_emitter
Python .pyc      →  pyc_parser → py_reconstruct  → py_emitter
Lua .luac        →  lua_parser →                 → lua_emitter
WASM .wasm       →  wasm_parser →                → wat_emitter
PTX .ptx         →  ptx_parser →  ptx_lifter     → CUDA C (inline in C emitter)
```

### AI Inference Pipeline

```
User prompt (+ decompiled context)
    │
    ▼
Qwen3Tokenizer::encodeChat()
    │  token IDs
    ▼
Qwen3Pipeline::prefill(token_ids)
    │  KV cache populated
    ▼
Generate loop:
  Qwen3Pipeline::forward(next_token, pos)
    ├── Embedding lookup
    ├── For each layer: RMSNorm → QKV → RoPE → FlashAttn → FFN (MoE)
    └── LM head → logits
  Qwen3Sampler::sample(logits)
    │  next token ID
    ▼
  Qwen3Tokenizer::decode(token_id) → text fragment
    │
    ▼
  StreamCallback → InferenceWorker::tokenGenerated signal
    │
    ▼
  AIAssistantPanel::onTokenGenerated() → append to chat bubble
    │
    ▼
  [until EOS or max_new_tokens]
```
