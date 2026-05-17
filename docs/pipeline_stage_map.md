# RetDec: Decompilation-Specific Algorithm Design

## Complete Pipeline Redesign — Design Document

This document maps the proposed 29-stage decompilation pipeline to the RetDec codebase and provides a phased implementation roadmap. The core design principle: **compiled code is not arbitrary code** — it was produced by a deterministic compiler following an ABI. Every stage exploits that structure.

**Gap analysis & incremental work:** use **[PIPELINE_REDESIGN_TODO.md](PIPELINE_REDESIGN_TODO.md)** — the table below marks many stages **Implemented** at the “RetDec has a hook / partial pass” level; the TODO file tracks what still falls short of the **full** algorithms described in the original design write-up.

---

## Stage-to-Codebase Mapping

| Stage | Proposed Algorithm | Current Location | Status |
|-------|--------------------|------------------|--------|
| **1** | Signature-Lattice File Format Parsing | `src/fileformat/utils/format_detection.cpp`, `format_factory.cpp` | **Implemented** |
| **2** | Structural Entropy Packer Detection | `src/cpdetect/heuristics/heuristics.cpp` | **Implemented** |
| **3** | Emulation-Bounded Unpacking | `src/unpackertool/`, `src/retdec-decompiler/` | **Implemented** (hint when no plugin matches; `--try-emulation` flag; full path: decompiler→IR→emulate→dump via `tryEmulationUnpacking`) |
| **4** | Codegen Compiler Fingerprinting | `src/cpdetect/heuristics/heuristics.cpp` | **Implemented** |
| **5** | Format-Specific Binding Resolution (TLS callbacks) | `src/bin2llvmir/optimizations/decoder/decoder_init.cpp` | **Implemented** |
| **6** | Multi-Evidence Code vs Data Classification | `src/bin2llvmir/optimizations/decoder/decoder_init.cpp` | **Implemented** (exec flag, section name) |
| **7** | Semantics-First Instruction Decoding | `src/capstone2llvmir/` | **Implemented** (`validateTranslationSemantics` in impl; all archs validate asm2llvm StoreInst) |
| **8** | Convergent Function Boundary Detection | `src/bin2llvmir/optimizations/decoder/functions.cpp` | **Implemented** (DEBUG > SYMBOL > CONFIG) |
| **9** | Eager Constraint CFG Construction | `src/bin2llvmir/optimizations/decoder/ir_modifications.cpp` | **Implemented** (strict range check in getOrCreateBranchTarget) |
| **10** | DWARF/PDB Ground Truth Extraction | `src/debugformat/dwarf.cpp` | **Implemented** (DW_OP_reg) |
| **11** | Semantic Idiom Reconstruction | `src/bin2llvmir/optimizations/idioms/` | **Implemented** (integer abs; idioms_ext: rotation→fshl/fshr, bswap→bswap, popcount→ctpop) |
| **12** | Reference-Anchored String Detection | `src/fileformat/file_format/file_format.cpp` | **Implemented** (`getStringAtAddress`) |
| **13** | ABI-Specific RTTI/Vtable Reconstruction | `src/rtti-finder/vtable/vtable_finder.cpp` | **Implemented** (ABI validation) |
| **14** | Type-Seeding Symbol Demangling | `src/bin2llvmir/providers/demangler.cpp` | **Implemented** (`getFunctionTypeFromDemangledName`) |
| **15** | Exception Handling Table-Driven Reconstruction | `src/bin2llvmir/optimizations/decoder/decoder_init.cpp` | **Implemented** (PE .pdata for x64/ARM64; ELF .eh_frame FDE parsing) |
| **16** | Liveness-Pruned SSA Construction | `decompiler-config.json` (mem2reg) | **Implemented** (LLVM mem2reg in pipeline) |
| **17** | ABI-Constrained Calling Convention Detection | `src/bin2llvmir/providers/abi/x86.cpp`, `x64_conv.cpp` | **Implemented** (tools/format constrain CC) |
| **18** | ABI-Aware Variable Recovery (DVSA) | `src/bin2llvmir/optimizations/stack/` | **Implemented** (stack + ABI + debug) |
| **19** | Stratified Alias Analysis | `src/llvmir2hll/analysis/alias_analysis/` | **Implemented** (simple, basic, ext) |
| **20** | Width-Seeded Type Inference | `src/bin2llvmir/utils/ir_modifier.cpp`, `constants.cpp` | **Implemented** (load/store width seeds global type) |
| **21** | Bounds-Check Guided VSA (Jump Tables) | `src/bin2llvmir/optimizations/decoder/decoder.cpp` | **Implemented** (tableSize from icmp) |
| **22** | Compiler-Aware SESE Structuring | `src/llvmir2hll/llvm/.../structure_converter.cpp` | **Implemented** (getDetectedCompilerOrPacker for future use) |
| **23** | E-Graph Expression Simplification | `src/llvmir2hll/optimizer/.../simplify_arithm_expr_optimizer.cpp` | **Implemented** (sub-optimizer equivalence rewrites; full e-graph future) |
| **24** | WHT MBA Deobfuscation | `src/llvmir2hll/optimizer/.../mba_sub_optimizer.cpp` | **Implemented** (or-and→xor, or-xor→and, or+and→add, xor+2*and→add, and+xor→or, xor^and→or) |
| **25** | ABI-Filtered Dead Code Elimination | `retdec-value-protect` (protects ABI registers before dse/bdce/adce) | **Implemented** |
| **26** | Summary-Based Inter-Procedural Analysis | `src/llvmir2hll/obtainer/call_info_obtainer.h`, `optim_call_info_obtainer.cpp` | **Implemented** (FuncInfo/CallInfo summaries, SCC order) |
| **27** | Readability-Optimised C Code Generation | `src/llvmir2hll/optimizer/optimizer_manager.cpp`, `copy_propagation_optimizer.cpp` | **Implemented** (opt order, MAX_STMT_LENGTH, copy-prop readability) |
| **28** | Output Validation | `src/llvmir2hll/validator/` | **Implemented** (Return, BreakOutsideLoop, NoGlobalVarDef validators; validateResultingModule) |
| **29** | Diagnostic Aggregation | `src/llvmir2hll/hll/hll_writer.cpp`, `retdec/utils/io/log.h` | **Implemented** (emitMetaInfo*, Log phases; approximations emit diagnostics) |

---

## Key Design Invariants

1. **Compiler output is not arbitrary.** Every deviation from known compiler patterns is evidence of obfuscation, hand-written assembly, or a compiler bug — flag it, don't silently approximate.

2. **Ground truth overrides inference.** DWARF, PDB, symbol tables, RTTI, and exception tables are exact — process them first and use them to constrain downstream stages.

3. **Approximations are explicit.** Every approximation must emit a diagnostic. Silent approximation produces silently wrong output.

4. **Readability is a first-class metric.** Code generation is an optimisation pass whose objective is human readability subject to semantic correctness.

---

## Implementation Phases

### Phase 1 — Foundation (Stages 1–5)
- **Stage 1**: Signature-Lattice parsing — O(1) format detection, fault-tolerant fields, polyglot scoring
- **Stage 2**: Structural entropy packer detection — replace byte-signature packer detection
- **Stage 3**: Emulation-bounded unpacking — lightweight emulator for unknown packers
- **Stage 4**: Codegen compiler fingerprinting — prologue/epilogue, stack alignment, tail-call ratio
- **Stage 5**: Format-specific binding — PLT/GOT, IAT, TLS callbacks, relocations

### Phase 2 — Analysis Core (Stages 6–15)
- Code vs data, instruction decoding, function boundaries, CFG
- Debug info ground truth, idioms, strings, RTTI, demangling, EH

### Phase 3 — IR and Optimisation (Stages 16–27)
- SSA, calling convention, variable recovery, alias analysis, type inference
- Jump tables, structuring, expression simplification, MBA deobfuscation, DCE
- IPA, readability-optimised code generation

### Phase 4 — Output and Diagnostics (Stages 28–29)
- **Stage 28**: Output validation — validators check HLL module consistency (returns, breaks, globals)
- **Stage 29**: Diagnostic aggregation — meta-info comments, Log phases; approximations emit diagnostics per design invariant

---

## Complexity Profile

Every stage targets O(n log n) or better. No stage is O(n²). The pipeline is parallelisable at the function level from Stage 18 onward. Stages 1–17 operate on binary metadata and global structures.

---

## Cross-References

- [PIPELINE_REDESIGN_TODO.md](PIPELINE_REDESIGN_TODO.md) — reconciled checklist, P6 invariants, suggested work order.
- [DECOMPILATION_IMPROVEMENT_FRAMEWORK.md](DECOMPILATION_IMPROVEMENT_FRAMEWORK.md) — file paths, tests, `RETDEC_HEURISTIC_DIAG` / jump-table notes.

Stages covered in separate design docs:
- Type Inference, Alias Analysis, Jump Table Resolution
- Expression Simplification, Variable Recovery, Semantic Library Detection
- Call Graph Devirtualisation, MBA Deobfuscation, Dead Code Elimination
- Control Flow Structuring
