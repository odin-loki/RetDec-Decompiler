# RetDec Architecture Guide

RetDec Imortek **2.0.22** architecture as implemented in this tree. This
document describes the **running** decompiler, not the proposed 29-stage
redesign. For a stage-by-stage map of that redesign onto real files, see
[pipeline_stage_map.md](pipeline_stage_map.md).

**Companion docs:** [BUILD_REFERENCE.md](BUILD_REFERENCE.md) ·
[developer_guide.md](developer_guide.md) · [user_manual.md](user_manual.md) ·
[docs/README.md](README.md).

---

## Table of Contents

1. [High-Level Overview](#overview)
2. [Build System and Module Layout](#modules)
3. [Running Pipeline](#pipeline)
4. [Managed-Language Dispatch](#managed)
5. [Core Library Details](#libraries)
6. [Qt GUI Architecture](#gui)
7. [AI Inference Engine](#ai)
8. [Plugin System](#plugins)
9. [Performance and Threading Model](#threading)
10. [Design Decisions](#decisions)
11. [Data Flow Diagrams](#diagrams)

---

## High-Level Overview {#overview}

RetDec is a retargetable machine-code decompiler. Output is **input-keyed**:

| Input | Emitter path | Output |
|-------|----------------|--------|
| Native ELF / PE / Mach-O / COFF / Intel HEX / raw | `bin2llvmir` → `llvmir2hll` C writer | **C** (`.c`) |
| Python `.pyc` | `pyc_parser` + `py_reconstruct` → `py_emitter` | Python |
| Lua `.luac` | `lua_parser` → `lua_emitter` | Lua |
| WebAssembly `.wasm` | `wasm_parser` → `wat_emitter` | WAT |
| JVM `.class` / `.jar` | `jvm_parser` + `jvm_reconstruct` → `java_emitter` | Java |
| Android `.dex` / `.apk` | `dex_parser` → `java_emitter` | Java |
| .NET CLI PE | `cli_parser` → `csharp_emitter` | C# |

There is **no** general `--output-lang cpp`. The CLI rejects `cpp` / `c++` /
`cxx` (`src/retdec-decompiler/output_lang.cpp`). `src/cxx_backend/` exists
and is unit-tested; it is not a shipped native writer.

In-tree emitters that are **not** wired from `decompileManaged()`:

- `src/fsharp_emitter/` — F# writer, tests only
- `src/vbnet_emitter/` — VB.NET writer, tests only
- `src/kotlin_emitter/` — Kotlin writer, tests only (JVM/DEX still emit Java)
- PTX → CUDA-C (`src/ptx_decompile/` parser + lifter) — no CLI path accepts `.ptx`

Native `--output-lang python|csharp|java|wat` is accepted by the CLI but the
native LLVM path still emits C (`applyNativeOutputLanguage` forces the `c`
HLL writer). Managed inputs ignore `--output-lang` and use the table above.

**LLVM pin:** `cmake/deps.cmake` fetches
`llvmorg-23.1.0` (`llvm-project-23.1.0.src.tar.xz`). Do not treat this as
LLVM 8.

Default native extras:

- **Buildable C sidecar** on by default: `--buildable` /
  `RETDEC_EMIT_BUILDABLE` writes `.h`, `_stubs.c`, and `.buildable.c` next to
  the unchanged `.c`. Opt out with `--no-buildable` or
  `RETDEC_EMIT_BUILDABLE=0`.
- **Post-pipeline semantic detectors** (comments + `.config.json`), not
  production-quality STL reconstruction. Name-blind algorithm-recovery F1 on
  the 216-binary corpus is **0.056** ([BENCHMARKS.md](BENCHMARKS.md)).
- **Optional llama.cpp refine** (`RETDEC_ENABLE_LLAMACPP`, runtime
  `RETDEC_NEURAL_REFINE`). See [NEURAL_REFINEMENT.md](NEURAL_REFINEMENT.md).

Native CPU architectures: see [ARCHITECTURE_TARGETS.md](ARCHITECTURE_TARGETS.md).
Compiler integer, FP, SIMD, and atomics lift to LLVM IR for x86 /
x86-64, ARM / Thumb, ARM64, MIPS 32/64, PIC32, PowerPC 32/64, RISC-V 32/64,
SPARC 32/64, SystemZ (64), and XCore (32) at the same gcc `-O1`/`-O2` bar
as x86-64 (including FMLA, microMIPS/R6 compact, VIS, MVC, channel IN/OUT).
Leftover specialized IDs (SVE, AES/SHA on every ISA including x86, QPX,
unprinted SASS opcodes) still become pseudo-asm. **SASS is not Production**
(cubin/fatbin probe, not `-a sass`).

---

## Build System and Module Layout {#modules}

Root `CMakeLists.txt` requires **CMake 3.13**. Root `CMakePresets.json` and
`cmake/superbuild/` require **CMake 3.26**. Presets are the documented
developer path; a direct `cmake -S . -B build` can still use 3.13.

Dependencies are ExternalProject pins in `cmake/deps.cmake`. `vcpkg.json` is
a leftover manifest that no CMake file, preset, script, or workflow reads.

```
RetDec/
├── CMakeLists.txt            Root project (VERSION 2.0.22, cmake 3.13)
├── CMakePresets.json         core/full debug and release, asan, coverage
├── cmake/deps.cmake          LLVM 23.1.0, Capstone 5.0.9, llama.cpp b10451, …
├── include/retdec/           Public headers
├── src/                      Implementations (see developer_guide.md)
├── tests/                    Unit, integration, managed, algorithm-recovery
└── docs/
```

`include/retdec/` does **not** 1:1-mirror `src/`. Many `src/` libraries have
headers there; tools (`retdec-decompiler`, `fileinfo`, …) do not.

### What actually links into `retdec-decompiler`

Native decompile (`src/retdec/retdec.cpp`) links:

`bin2llvmir`, `llvmir2hll`, `llvmir-emul`, `config`, `fileformat`,
`concurrency_detect`, `sort_detect`, `container_detect`, `algo_recover`,
`type_inference`, `crypto_detect`, `serial_detect`, `pattern_detect`, `ipa`,
`call_conv`, `ptx_decompile` (OpenCL host recovery only), `ssa`, optional
`neural`, `gpu-scanner` (built; **not called** from `decompile()`),
`profiling`.

Managed dispatch (`src/retdec-decompiler/managed_decompiler.cpp`) additionally
links: `jvm_parser`, `jvm_reconstruct`, `java_emitter`, `cli_parser`,
`csharp_emitter`, `dex_parser`, `pyc_parser`, `py_reconstruct`, `py_emitter`,
`lua_parser`, `wasm_parser`. It includes `cil_reconstruct` types but passes
an **empty** reconstruction map into the C# emitter.

### Libraries the product does not link

Twelve library targets under `src/` are built, unit-tested and installed, and
reach no decompilation. `scripts/ci/check_link_graph.py` owns this list:

| Library | Directory | Why it is not in the pipeline |
| --- | --- | --- |
| `retdec-cfg` | `src/cfg` | CFG reconstruction; the pipeline uses bin2llvmir's own CFG |
| `retdec-code-data` | `src/code_data` | code/data separation; the loader decides that today |
| `retdec-compiler-abi` | `src/compiler_abi` | ABI tables; `param_return` in bin2llvmir carries its own |
| `retdec-compiler-detect` | `src/compiler_detect` | compiler identification; `cpdetect` is what runs |
| `retdec-eh-reconstruct` | `src/eh_reconstruct` | exception-handler recovery, not consumed by any emitter |
| `retdec-func-boundary` | `src/func_boundary` | function boundary detection; the decoder finds functions |
| `retdec-idiom-reconstruct` | `src/idiom_reconstruct` | idiom recovery; bin2llvmir `retdec-idioms` is what runs |
| `retdec-loader-sim` | `src/loader_sim` | loader simulation, used only by its own tests |
| `retdec-module-cluster` | `src/module_cluster` | module clustering, no caller |
| `retdec-rtti` | `src/rtti` | RTTI reconstruction; `rtti-finder` is the one bin2llvmir links |
| `retdec-string-detect` | `src/string_detect` | string classification, no caller |
| `retdec-testing` | `src/testing` | test support library |

`retdec-experimental` (`src/experimental`) is linked by nothing, tests
included; it is the task scaffold behind `RETDEC_ENABLE_EXPERIMENTAL_SCAFFOLD`.

Also unwired from the CLI (built, tested, not a native/managed target):

- `src/cxx_backend/` + `src/codegen/` (C++ writer / alternate C emitter)
- `src/fsharp_emitter/`, `src/vbnet_emitter/`, `src/kotlin_emitter/`
- `src/cuda_accel/`, `src/opencl/` (parked GPU accel; OpenCL is not added
  from `src/CMakeLists.txt`)
- `src/debug_info/` (`pdb_extractor.cpp` LLVM PDB reader; the live PDB path
  is `debugformat` + `pdbparser`)

---

## Running Pipeline {#pipeline}

There is one native LLVM pass pipeline, then optional post-passes. It is
**not** the 29-stage table in older drafts of this file.

### 0. CLI dispatch (`retdec-decompiler`)

`src/retdec-decompiler/retdec-decompiler.cpp`:

1. Parse flags (`--output-lang`, `--buildable`, `--pdb FILE`, `-a`, …).
2. Probe input bytes. If a managed format matches, call `decompileManaged()`
   and **return** (no `bin2llvmir`).
3. Optional unpacker. `--try-emulation` is opt-in when no unpacker plugin
   matches (`tryEmulationUnpacking`).
4. `retdec::decompile(config)` for native binaries.

### 1. LLVM pass manager (`decompiler-config.json`)

`src/retdec/retdec.cpp` builds `llvm::legacy::PassManager` from
`decompParams.llvmPasses`. Default order (abbreviated):

```
retdec-provider-init
retdec-decoder                 # Capstone lift via capstone2llvmir
retdec-x86-addr-spaces, retdec-x87-fpu, retdec-main-detection
retdec-idioms-libgcc, retdec-inst-opt, retdec-cond-branch-opt
retdec-syscalls, retdec-stack, retdec-constants
retdec-param-return, retdec-simple-types
retdec-jump-table-recovery, retdec-class-hierarchy
retdec-value-protect + LLVM simplifycfg / mem2reg / instcombine / dse / …
retdec-idioms
retdec-llvmir2hll              # C emission
```

Front-end lifting is `src/capstone2llvmir/` (x86, ARM/Thumb, ARM64, MIPS,
PowerPC). Middle-end and C back-end are `src/bin2llvmir/` and
`src/llvmir2hll/`. Adding an architecture means a new Capstone translator
**and** ABI / decoder coverage; the HLL writer is shared.

### 2. Post-pipeline analysis (`retdec.cpp`)

After `pm.run()`, the decompiler rebuilds a lightweight
`retdec::ssa::SSAModule` from LLVM IR (`src/retdec/llvm_to_ssa.cpp`) and
runs detectors **if** `RETDEC_SKIP_SEMANTIC_RECOVERY` is unset:

| Step | Library | Notes |
|------|---------|--------|
| Calling convention | `src/call_conv` | `CallConvPass::runAll` |
| IPA | `src/ipa` | summaries / inline candidates; does not rewrite C |
| Type inference | `src/type_inference` | **only if** `RETDEC_TYPE_INFERENCE=1` |
| Container / algo / idiom / sort | `container_detect`, `algo_recover`, `sort_detect` | per-function; optional incremental cache |
| Concurrency | `concurrency_detect` | module-wide |
| Crypto / serial / design patterns | `crypto_detect`, `serial_detect`, `pattern_detect` | annotations |
| Export | `semantic_recovery_export.cpp` | JSON sidecar + `// [RetDec]` comments |
| Neural refine | `src/neural` | opt-in env; writes `*.refined.c` |
| Buildable sidecar | `maybeWriteBuildableSidecars` | default on |
| OpenCL host | `ptx_decompile::OclHostRecovery` | log-only if `cl*` APIs seen |

`ptx_decompile::CudaHostRecovery` (`cudaLaunchKernel` / `cuLaunchKernel`
detectors) is **implemented and unit-tested** and is **not** constructed
from `decompile()`. CUDA host recovery is therefore not a shipped pipeline
stage.

Standalone `src/ssa`, `src/cfg_structure`, `src/var_recovery`, `src/dce`,
and `src/codegen` are **not** the llvmir2hll C writer. llvmir2hll has its
own structuring (`structure_converter.cpp`), alias analyses, and validators.

---

## Managed-Language Dispatch {#managed}

Bypass the LLVM pipeline. Probe in
`src/retdec-decompiler/managed_decompiler.cpp`
(`detectManagedFormatFromBytes`):

| Magic / probe | Format | Writer |
|---------------|--------|--------|
| Java class `CAFEBABE` (with lattice vs Mach-O fat) | `JavaClass` | `JavaFileEmitter` |
| ZIP + JAR | `JavaJar` | `JavaFileEmitter` |
| `dex\n` | `Dex` | `JavaFileEmitter` |
| ZIP + APK | `Apk` | `JavaFileEmitter` |
| WASM `\0asm` | `Wasm` | `WatEmitter` |
| Lua `\x1bLua` | `LuaBytecode` | `lua_emitter` |
| CPython marshal magic | `PythonPyc` | `PyFileEmitter` |
| MZ + CLI metadata | `CliAssembly` | `CsFileEmitter` (`cil_reconstruct` is **linked** via `csharp_emitter` but `decompileCliAssembly` passes an empty result map — reconstructor not run) |

Default language hints (`managedOutputLangHint`): java / python / lua /
wat / csharp. There is no `fsharp`, `vbnet`, or `kotlin` route in this
function.

---

## Core Library Details {#libraries}

### `concurrency_detect` (post-pipeline)

Seven detector classes implementing `IConcurrencyDetector`
(`include/retdec/concurrency_detect/concurrency_detect.h`):

| Class | Detects |
|-------|---------|
| `StdThreadDetector` | `std::thread`, `std::mutex`, `std::condition_variable` |
| `PthreadDetector` | `pthread_create`, `pthread_mutex_*`, `pthread_cond_*`, semaphores |
| `Win32ThreadDetector` | `CreateThread`, `WaitForSingleObject`, CRITICAL_SECTION, events |
| `AtomicDetector` | `std::atomic<T>`, `__atomic_*` builtins, `LOCK XCHG` patterns |
| `SpinlockDetector` | Compare-and-swap loops, `__sync_bool_compare_and_swap` |
| `OpenMPDetector` | `__kmpc_fork_call`, `omp_get_thread_num` |
| `TBBDetector` | `tbb::parallel_for`, `tbb::task_group`, `tbb::concurrent_vector` |

`ConcurrencyDetector` orchestrates them. Results become
`semanticDetections` comments/JSON. Detection is pattern-based; do not
treat it as complete concurrency recovery.

### `ptx_decompile`

**`PtxParser` / `InstrLifter` / `ThreadIndexRecovery`:** PTX text → CUDA-C
strings. Unit-tested. Nothing in `retdec-decompiler` feeds a `.ptx` file
here.

**`OclHostRecovery`:** wired from `decompile()`; looks for `clCreate*` /
`clEnqueue*` / etc. on the post-pipeline SSA module. Emits a log summary.

**`CudaHostRecovery`:** five detectors (`KernelLaunchDetector`,
`CudaMemoryDetector`, `CudaDeviceDetector`, `CudaStreamEventDetector`,
`NvccStubDetector`). Tests only; not called from `decompile()`.

This is **host API recovery**, not GPU acceleration. See
[CUDA_CAPABILITIES.md](CUDA_CAPABILITIES.md).

### `module_cluster`

Louvain clustering, header inference, `CMakeEmitter`. **No caller** in the
decompiler (test-only library). GUI Analysis → “Module clustering” is an
in-process settings checkbox; F5 still runs `retdec-decompiler` and does
not invoke this library.

### Semantic detectors (post-pipeline, low F1)

Documented further in [SEMANTIC_OUTPUT.md](SEMANTIC_OUTPUT.md).

- `container_detect`: vector, list, map, unordered_map, string, ring buffer
- `algo_recover`: transform, find, binary search, partition, accumulate, …
- `sort_detect`: introsort, mergesort, heapsort, quicksort, bubblesort, radix
- `crypto_detect`: AES, SHA-1/256, ChaCha20, Salsa20, RSA/DH, RC4, MD5, CRC, …
- `serial_detect`: Protobuf, FlatBuffers, JSON, XML (symbol + structural)
- `pattern_detect`: Singleton, Factory, and related design-pattern heuristics

Labels are recovery **hints**. Default C output stays pointers/structs.

### `profiling`

Thread-safe `Profiler` with `std::mutex`. `RssTracker`: Linux
`/proc/self/status` `VmPeak:`; macOS `getrusage` + Mach; Windows
`GetProcessMemoryInfo`. Used around pipeline phases in `retdec.cpp`.

### `testing`

`TestBinary` builds trivial ELF64/ELF32/PE32/raw fixtures.
`SnapshotTester` uses FNV-1a 64-bit.

---

## Qt GUI Architecture {#gui}

`retdec-gui` is optional (`RETDEC_REQUIRE_QT6` or Qt6 found). Decompilation
is **not** in-process: `RetDecMainWindow` starts `retdec-decompiler` as a
`QProcess` (`src/gui/decompiler_launch.cpp`).

### Window layout (`src/gui/mainwindow.cpp`)

```
MainWindow
  ├── documentTabs_          Decompiled C | Assembly | IR (SSA) | CFG | Synced tri-pane
  ├── workspaceTabWidget_    Strings | Inspect | Binary | Target
  ├── outputTabs_            Console | Problems | History | Progress
  └── tool windows           Function list, Call Graph, Type Hierarchy,
                             AI Assistant (Tools menu; not a default dock)
```

`AnalysisBridge` reports progress to `ProgressPanel`. It does **not** run
pipeline stages on `QThreadPool`.

### SettingsDialog

Eight tabs (`src/gui/panels/settings_dialog.cpp`): General, Analysis, **CUDA**,
ML, Recovery, Advanced, Decompiler, Plugins. There is **no** OpenCL tab.
Analysis checkboxes (module clustering, C++ lifter, CUDA host recovery) are
GUI-only in-process toggles; F5 still uses the CLI pipeline above.

`AppSettings` uses `QSettings(QSettings::IniFormat, QSettings::UserScope,
"retdec", "settings")` — INI on every platform. Typical paths:
`~/.config/retdec/settings.ini` (Linux) and
`%APPDATA%/retdec/settings.ini` (Windows). Not the Windows registry.

### Plugin System {#plugins}

```
Plugin file (.so / .dll)
  exports:
    retdec_create_plugin()      → IRetDecPlugin*
    retdec_destroy_plugin(p)
    retdec_plugin_api_version() → const char*  (must == "1.0")
```

`PluginManager` uses `QPluginLoader`, checks `RETDEC_PLUGIN_API_VERSION`
(`"1.0"`), and topological-sorts declared dependencies.

| Interface | Hook |
|-----------|------|
| `IDecompilerPlugin` | `runStage(PipelineContext&)` |
| `IOutputPlugin` | `transform(decompiledC)` |
| `IVisualisationPlugin` | `createPanel(parent)` |
| `IAnalysisPlugin` | `analyse(PipelineContext&)` |

Plugins do not insert LLVM passes into `decompiler-config.json`.

---

## AI Inference Engine {#ai}

**Not in the default decompiler pipeline.** There is no `src/qwen3/`.

Two separate consumers:

1. **Post-decompile refine** — `neural::maybeRefineDecompilerOutput` when
   `RETDEC_NEURAL_REFINE=1` and a GGUF path is set. Requires
   `RETDEC_ENABLE_LLAMACPP` at build time for a real backend; otherwise the
   stub in `src/retdec/neural_refine_stub.cpp` is a no-op. Compile gate is
   `cc`/`gcc -fsyntax-only`. Differential gate is **not implemented**
   (`RETDEC_NEURAL_DIFF_GATE=1` warns and skips). See
   [NEURAL_REFINEMENT.md](NEURAL_REFINEMENT.md).
2. **GUI Tools → AI Assistant…** — `InferenceWorker` on a `QThread` loads a
   GGUF in-process when neural is linked. Independent of the refine sidecar.

```
maybeRefineDecompilerOutput
  ├── model_verify     SHA-256 allowlist (fails closed if empty)
  ├── llama.cpp        GGUF generate (optional n_gpu_layers)
  ├── gates            -fsyntax-only + tree-sitter AST (structural)
  └── applyJsonRenameMap  Naming-tier GBNF JSON map
```

No `retdec-qwen3-runner` and no CLI `--model`.

---

## Performance and Threading Model {#threading}

### Native decompile process

`decompile()` takes a process-wide `pipelineLock()` (bin2llvmir providers
are not re-entrant). LLVM passes run on the calling thread. Post-pipeline
per-function detectors may use `retdec::utils::ThreadPool` when
`RETDEC_PARALLEL_ANALYSIS` allows it and function count is high enough.

The GUI does not share that lock: it spawns another `retdec-decompiler`
process.

### OpenCL / CUDA accel

Parked `src/opencl/` is **not** added from `src/CMakeLists.txt`.
`src/cuda_accel/` is opt-in `RETDEC_ENABLE_CUDA_ACCEL` (default OFF) and is
not linked from `src/retdec`. `RETDEC_ENABLE_CUDA` (in
`src/utils/CMakeLists.txt`, default OFF) builds `GpuScanner`; nothing in
`decompile()` constructs one.

### Profiling overhead

`Profiler::measure(name)` takes a mutex on construction and destruction.
When disabled, the destructor skips the record.

---

## Design Decisions {#decisions}

### Why LLVM IR as the pivot format?

Native architectures lift to LLVM IR so llvmir2hll and LLVM scalar passes
are shared. Managed formats never enter that IR.

### Why a second SSA module after llvmir2hll?

Detectors in `src/algo_recover` and friends consume `retdec::ssa`, not
llvmir2hll BIR. `buildSsaModule` is an adapter. It does not replace
mem2reg / llvmir2hll.

### Why llama.cpp instead of a custom Qwen engine?

`src/qwen3/` was deleted. Optional refinement uses pinned llama.cpp
**b10451** in `cmake/deps.cmake`.

### Why INI for GUI settings?

`QSettings::IniFormat` is human-readable and the same format on Windows and
Unix.

---

## Data Flow Diagrams {#diagrams}

### Native pipeline (what `decompile()` runs)

```
Binary File
    │
    ▼
fileformat + loader + optional unpacker
    │
    ▼
LLVM PassManager (decompiler-config.json)
    decoder / capstone2llvmir → bin2llvmir opts → llvmir2hll C
    │
    ▼
Post-pipeline SSA rebuild
    call_conv → ipa → (optional type_inference)
    → container / algo / sort / concurrency / crypto / serial / patterns
    → comments + config JSON
    → optional llama.cpp refine (*.refined.c)
    → default --buildable sidecars
    → OclHostRecovery log
    │
    ▼
out.c  (native C; managed formats never reach here)
```

### Managed paths (bypass LLVM)

```
JVM .class/.jar  →  jvm_parser → jvm_reconstruct → java_emitter
Android .dex/.apk → dex_parser →                 → java_emitter
.NET CLI         →  cli_parser → (no cil_reconstruct run) → csharp_emitter
Python .pyc      →  pyc_parser → py_reconstruct  → py_emitter
Lua .luac        →  lua_parser →                 → lua_emitter
WASM .wasm       →  wasm_parser →                → wat_emitter
```

Not wired: F# / VB.NET / Kotlin emitters; PTX parser → CUDA-C; `cxx_backend`.
