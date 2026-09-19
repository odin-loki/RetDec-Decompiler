# RetDec: Decompilation-Specific Algorithm Design

## 29-stage redesign mapped onto the **2.0.22** tree

This table is a **design map**, not a second running pipeline. The shipped
native path is the LLVM pass list in
`src/retdec-decompiler/decompiler-config.json` plus post-pipeline detectors
in `src/retdec/retdec.cpp`. See [architecture.md](architecture.md).

Status words:

| Status | Meaning |
|--------|---------|
| **Implemented** | The named hook runs on the native (or documented opt-in) path |
| **Partial** | Code exists and is used, but the algorithm is a subset of the design write-up |
| **Not wired** | Library or function exists (often with unit tests) and is **not** called from `decompile()` / the CLI |

Do not read “Implemented” as “full algorithm, production F1”. Semantic
detectors that do run still score name-blind mean F1 **0.056** on the 216
binary corpus ([BENCHMARKS.md](BENCHMARKS.md)).

**Gap analysis:** [PIPELINE_REDESIGN_TODO.md](internal/PIPELINE_REDESIGN_TODO.md).

---

## Stage-to-Codebase Mapping

| Stage | Proposed algorithm | Current location | Status |
|-------|--------------------|------------------|--------|
| **1** | Signature-lattice file format parsing | `src/fileformat/utils/format_detection.cpp`, `format_factory.cpp` (`FormatLatticeHints`) | **Implemented** |
| **2** | Structural entropy packer detection | `src/cpdetect/heuristics/heuristics.cpp` | **Implemented** (entropy + import/W+X heuristics; not a full packer emulator) |
| **3** | Emulation-bounded unpacking | `src/unpackertool/`, `src/retdec-decompiler/` `tryEmulationUnpacking` | **Partial** — opt-in `--try-emulation` when no unpacker plugin matches; packed input is still the default fallback |
| **4** | Codegen compiler fingerprinting | `src/cpdetect/heuristics/heuristics.cpp` | **Implemented** (`cpdetect`; standalone `src/compiler_detect` is not linked) |
| **5** | Format-specific binding (TLS callbacks) | `src/bin2llvmir/optimizations/decoder/decoder_init.cpp` `initJumpTargetsTls` | **Implemented** (TLS callbacks as extra entry points) |
| **6** | Multi-evidence code vs data | `src/bin2llvmir/optimizations/decoder/decoder_init.cpp` | **Partial** (executable flag, section name). `src/code_data` is **not wired** |
| **7** | Semantics-first instruction decoding | `src/capstone2llvmir/` `validateTranslationSemantics` | **Partial** — default check is `isa<StoreInst>` on the translation result, not a full semantic oracle. SPARC/SystemZ/XCore throw |
| **8** | Convergent function boundary detection | `src/bin2llvmir/optimizations/decoder/functions.cpp` | **Implemented** (DEBUG > SYMBOL > CONFIG). `src/func_boundary` is **not wired** |
| **9** | Eager-constraint CFG construction | `src/bin2llvmir/optimizations/decoder/ir_modifications.cpp` `getOrCreateBranchTarget` | **Implemented** (decoder CFG). `src/cfg` is **not wired** |
| **10** | DWARF/PDB ground truth | `src/debugformat/dwarf.cpp` (`DW_OP_reg*`), `src/debugformat/pdb.cpp` + `src/pdbparser/` | **Partial** — DWARF/PDB names and types when present. CLI `--pdb` is a **file** (`checkFile` / `is_regular_file`), not a directory. `src/debug_info` is **not wired** |
| **11** | Semantic idiom reconstruction | `src/bin2llvmir/optimizations/idioms/` (`retdec-idioms`, `idioms_ext`: fshl/fshr, bswap, ctpop) | **Partial** — compiler idioms in bin2llvmir. `src/idiom_reconstruct` is **not wired** |
| **12** | Reference-anchored string detection | `src/fileformat/file_format/file_format.cpp` `getStringAtAddress` | **Implemented**. `src/string_detect` is **not wired** |
| **13** | ABI-specific RTTI/vtable | `src/rtti-finder/vtable/vtable_finder.cpp` | **Implemented**. `src/rtti` is **not wired** |
| **14** | Type-seeding symbol demangling | `src/bin2llvmir/providers/demangler.cpp` `getFunctionTypeFromDemangledName` | **Implemented** |
| **15** | Exception-handling table reconstruction | `src/bin2llvmir/optimizations/decoder/decoder_init.cpp` (`initJumpTargetsPdata`, `initJumpTargetsEhFrame`) | **Partial** — PE `.pdata` / ELF `.eh_frame` used as **function-boundary** hints, not `try`/`catch` emission. `src/eh_reconstruct` is **not wired** |
| **16** | Liveness-pruned SSA | LLVM `mem2reg` in `decompiler-config.json`; post-pipeline `src/retdec/llvm_to_ssa.cpp` | **Partial** — LLVM SSA for opts; `src/ssa` rebuilt **after** C emission for detectors only |
| **17** | ABI-constrained calling convention | `src/bin2llvmir/providers/abi/` (x86, x64, arm, arm64, mips, ppc, …) + post-pipeline `src/call_conv` | **Implemented** (tables + `CallConvPass`). Not every CC variant is covered |
| **18** | ABI-aware variable recovery (DVSA) | `src/bin2llvmir/optimizations/stack/` | **Partial** — stack + ABI + debug in bin2llvmir. `src/var_recovery` (DVSA) is **not wired** |
| **19** | Stratified alias analysis | `src/llvmir2hll/analysis/alias_analysis/` (`simple`, `basic`, `ext`) | **Implemented** in llvmir2hll. Standalone `src/alias_analysis` is **not** the C writer |
| **20** | Width-seeded type inference | `src/bin2llvmir` `retdec-simple-types`; optional `src/type_inference` | **Partial** — simple-types always runs. Standalone TypeInferencePass only if `RETDEC_TYPE_INFERENCE=1` |
| **21** | Bounds-check guided VSA (jump tables) | `src/bin2llvmir/optimizations/decoder/decoder.cpp` (`tableSize` from `icmp`) | **Partial** — x86-oriented table recovery; not a general VSA |
| **22** | Compiler-aware SESE structuring | `src/llvmir2hll/llvm/.../structure_converter.cpp` | **Partial** — llvmir2hll structuring. `getDetectedCompilerOrPacker()` exists and is **not** used to pick a SESE strategy. `src/cfg_structure` feeds `src/codegen` / `cxx_backend`, not the C writer |
| **23** | E-graph expression simplification | `src/llvmir2hll/optimizer/.../simplify_arithm_expr_optimizer.cpp` | **Partial** — sub-optimizer rewrites. No e-graph engine |
| **24** | WHT MBA deobfuscation | `src/llvmir2hll/optimizer/.../mba_sub_optimizer.cpp` | **Partial** — listed Boolean-arithmetic identities only. Not Walsh–Hadamard transform MBA |
| **25** | ABI-filtered dead code elimination | `retdec-value-protect` then LLVM `dse` / simplifycfg in `decompiler-config.json` | **Partial**. Standalone `src/dce` is consumed by `src/codegen`, not llvmir2hll |
| **26** | Summary-based IPA | `src/llvmir2hll/obtainer/` (`optim_call_info_obtainer.cpp`) and post-pipeline `src/ipa` | **Partial** — both run; IPA results do not rewrite emitted C |
| **27** | Readability-optimised C generation | `src/llvmir2hll/` (`optimizer_manager.cpp`, `copy_propagation_optimizer.cpp`, C HLL writer) | **Implemented** (C only). `src/codegen` is **not** the shipped writer; `--output-lang cpp` is rejected |
| **28** | Output validation | `src/llvmir2hll/validator/` (Return, BreakOutsideLoop, NoGlobalVarDef) via `validateResultingModule` | **Implemented** (HLL consistency, not semantic equivalence) |
| **29** | Diagnostic aggregation | `src/llvmir2hll/hll/hll_writer.cpp` `emitMetaInfo*`; `retdec/utils/io/log.h` | **Implemented** |

### Related work that is **not** one of the 29 stages

| Work | Location | Status |
|------|----------|--------|
| Post-pipeline STL/algo/crypto/serial/concurrency comments | `src/retdec/retdec.cpp` + detector libs | **Partial** — runs; low name-blind F1 |
| Buildable C sidecar | `--buildable` / `RETDEC_EMIT_BUILDABLE` | **Implemented** (default on) |
| llama.cpp refine | `src/neural/` | **Partial** — opt-in; compile gate yes; differential gate **not implemented** |
| CUDA host (`cudaLaunchKernel`) | `src/ptx_decompile/cuda_host_recover.cpp` `CudaHostRecovery` | **Not wired** (tests only) |
| OpenCL host (`cl*` APIs) | same file, `OclHostRecovery` | **Implemented** (log summary) |
| PTX → CUDA-C | `src/ptx_decompile/` parser + lifter | **Not wired** (no CLI `.ptx`) |
| C++ backend | `src/cxx_backend/` | **Not wired** |
| Module clustering / CMake emit | `src/module_cluster/` | **Not wired** |
| GPU acceleration | `src/cuda_accel/`, `src/opencl/`, `GpuScanner` | **Not wired** into `decompile()` |

---

## Key Design Invariants

1. **Compiler output is not arbitrary.** Flag deviations; do not silently
   invent high-level constructs in emitted C.
2. **Ground truth overrides inference.** DWARF, PDB, symbol tables, RTTI, and
   exception tables constrain decoding when they load.
3. **Approximations are explicit.** Meta-info comments and `Log` phases are
   the diagnostic channel; semantic JSON is a sidecar, not a rewrite of `.c`.
4. **Readability is a first-class metric** for llvmir2hll, subject to the C
   writer remaining the only native target.

---

## Implementation Phases (historical)

These phases described the original redesign order. They are **not** a
claim that Phases 1–4 are finished at the algorithm level above.

### Phase 1 — Foundation (Stages 1–5)

Format detection, packer heuristics, optional emulation unpack, cpdetect,
TLS entries.

### Phase 2 — Analysis core (Stages 6–15)

Decoder, boundaries, CFG, debug, idioms, strings, RTTI, demangling, EH
**as implemented in bin2llvmir / debugformat**, not the standalone
`src/func_boundary` / `src/cfg` / `src/rtti` / `src/eh_reconstruct` libs.

### Phase 3 — IR and optimisation (Stages 16–27)

LLVM mem2reg + bin2llvmir + llvmir2hll. Standalone `src/ssa` /
`cfg_structure` / `var_recovery` / `codegen` remain a parallel stack used
by tests and `cxx_backend`.

### Phase 4 — Output and diagnostics (Stages 28–29)

HLL validators and meta-info comments.

---

## Complexity Profile

The original write-up targeted O(n log n) per stage. That is a **goal**,
not a measured bound for every pass in this tree.

---

## Cross-References

- [PIPELINE_REDESIGN_TODO.md](internal/PIPELINE_REDESIGN_TODO.md)
- [architecture.md](architecture.md) — running pipeline
- [ARCHITECTURE_TARGETS.md](ARCHITECTURE_TARGETS.md) — native CPU targets
- [SEMANTIC_OUTPUT.md](SEMANTIC_OUTPUT.md) — detector honesty
